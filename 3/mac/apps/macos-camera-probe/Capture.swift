import AppKit
import AVFoundation
import ImageIO
import UniformTypeIdentifiers

func savePNG(_ buffer: CVPixelBuffer, to url: URL) throws {
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    guard let base = CVPixelBufferGetBaseAddress(buffer),
          let context = CGContext(data: base, width: CVPixelBufferGetWidth(buffer), height: CVPixelBufferGetHeight(buffer),
            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue),
          let image = context.makeImage(),
          let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        throw NSError(domain: "CameraProbe.PNG", code: 1)
    }
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { throw NSError(domain: "CameraProbe.PNG", code: 2) }
}

final class Receiver: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let queue = DispatchQueue(label: "CameraProbe.capture")
    let destination: URL
    let checkBars: Bool
    let allowStatic: Bool
    var warmup: Int
    var frames = 0
    var changes = Set<UInt64>()
    var lastPTS = CMTime.invalid
    var firstPTS = CMTime.invalid
    var errors = Set<String>()
    var dimensions = Set<String>()
    var firstBytes: [[Int]] = []
    init(destination: URL, checkBars: Bool, allowStatic: Bool, warmup: Int) { self.destination = destination; self.checkBars = checkBars; self.allowStatic = allowStatic; self.warmup = warmup }
    var count: Int { queue.sync { frames } }
    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        if warmup > 0 { warmup -= 1; return }
        guard let buffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { errors.insert("missing image buffer"); return }
        let w = CVPixelBufferGetWidth(buffer), h = CVPixelBufferGetHeight(buffer)
        dimensions.insert("\(w)x\(h)")
        if w != TestPattern.width || h != TestPattern.height { errors.insert("unexpected dimensions") }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        if !pts.isValid { errors.insert("invalid timestamp") }
        if lastPTS.isValid && CMTimeCompare(pts, lastPTS) <= 0 { errors.insert("nonmonotonic timestamp") }
        if !firstPTS.isValid { firstPTS = pts }
        lastPTS = pts
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        if let base = CVPixelBufferGetBaseAddress(buffer)?.assumingMemoryBound(to: UInt8.self) {
            let stride = CVPixelBufferGetBytesPerRow(buffer)
            var hash: UInt64 = 14695981039346656037
            for index in Swift.stride(from: 0, to: stride * h, by: 997) { hash = (hash ^ UInt64(base[index])) &* 1099511628211 }
            changes.insert(hash)
            if frames == 0 && checkBars {
                for index in 0..<8 {
                    let offset = h / 2 * stride + ((2 * index + 1) * w / 16) * 4
                    let actual = [Int(base[offset+2]), Int(base[offset+1]), Int(base[offset]), Int(base[offset+3])]
                    firstBytes.append(actual)
                    let c = TestPattern.colors[index]
                    let expected = [Int(c.0 * 255), Int(c.1 * 255), Int(c.2 * 255), 255]
                    if zip(actual, expected).contains(where: { abs($0 - $1) > 6 }) { errors.insert("color bar \(index) mismatch") }
                }
            }
        } else { errors.insert("unreadable pixel buffer") }
        CVPixelBufferUnlockBaseAddress(buffer, .readOnly)
        if frames == 0 {
            do { try savePNG(buffer, to: destination.appendingPathComponent("first-frame.png")) }
            catch { errors.insert("snapshot failed: \(error)") }
        }
        frames += 1
    }
    func result(device: AVCaptureDevice, minimum: Int) -> [String: Any] {
        queue.sync {
            let duration = firstPTS.isValid && lastPTS.isValid ? CMTimeGetSeconds(CMTimeSubtract(lastPTS, firstPTS)) : 0
            if frames < minimum { errors.insert("insufficient frames") }
            if !allowStatic && changes.count < 2 { errors.insert("image did not change") }
            return ["passed": errors.isEmpty, "deviceName": device.localizedName, "deviceID": device.uniqueID,
                "frames": frames, "distinctFrameHashes": changes.count, "dimensions": dimensions.sorted(),
                "durationSeconds": duration, "measuredFPS": duration > 0 ? Double(frames - 1) / duration : 0,
                "colorBarsRGBA": firstBytes, "errors": errors.sorted()]
        }
    }
}

@main enum CaptureMain {
    static func main() {
        let args = CommandLine.arguments
        func option(_ name: String, _ fallback: String) -> String {
            guard let i = args.firstIndex(of: name), i+1 < args.count else { return fallback }
            return args[i+1]
        }
        let destination = URL(fileURLWithPath: option("--output", NSTemporaryDirectory() + "vtrpon-camera-capture"), isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
            if args.contains("--pattern-check") {
                var buffer: CVPixelBuffer?
                let attrs = [kCVPixelBufferIOSurfacePropertiesKey as String: [:]] as CFDictionary
                guard CVPixelBufferCreate(nil, TestPattern.width, TestPattern.height, kCVPixelFormatType_32BGRA, attrs, &buffer) == kCVReturnSuccess, let buffer else { throw NSError(domain: "CameraProbe.Buffer", code: 1) }
                try TestPattern.draw(buffer, frame: 75)
                try savePNG(buffer, to: destination.appendingPathComponent("pattern.png"))
                print("Offline test pattern saved; this does not test camera activation.")
                return
            }
            _ = NSApplication.shared
            let minimum = max(60, min(900, Int(option("--frames", "90")) ?? 90))
            let devices: [AVCaptureDevice]
            if #available(macOS 14.0, *) {
                devices = AVCaptureDevice.DiscoverySession(deviceTypes: [.external, .builtInWideAngleCamera], mediaType: .video, position: .unspecified).devices
            } else { devices = AVCaptureDevice.DiscoverySession(deviceTypes: [.externalUnknown, .builtInWideAngleCamera], mediaType: .video, position: .unspecified).devices }
            let inventory = devices.map { ["name": $0.localizedName, "id": $0.uniqueID] }
            try JSONSerialization.data(withJSONObject: inventory, options: [.prettyPrinted, .sortedKeys]).write(to: destination.appendingPathComponent("devices.json"))
            // Never select or open a physical camera as a fallback.
            let expectedID = option("--device-id", TestPattern.deviceID.uuidString)
            let expectedName = option("--device-name", TestPattern.name)
            guard let device = devices.first(where: { $0.uniqueID.caseInsensitiveCompare(expectedID) == .orderedSame && $0.localizedName == expectedName }) else {
                throw NSError(domain: "CameraProbe", code: 1, userInfo: [NSLocalizedDescriptionKey: "VTR-PON Camera Test is not available. Enable its camera extension first."])
            }
            var allowed = AVCaptureDevice.authorizationStatus(for: .video) == .authorized
            if AVCaptureDevice.authorizationStatus(for: .video) == .notDetermined {
                let semaphore = DispatchSemaphore(value: 0)
                AVCaptureDevice.requestAccess(for: .video) { granted in allowed = granted; semaphore.signal() }
                guard semaphore.wait(timeout: .now() + 90) == .success else { throw NSError(domain: "CameraProbe.Permission", code: 2) }
            }
            guard allowed else { throw NSError(domain: "CameraProbe.Permission", code: 1) }
            let session = AVCaptureSession()
            session.beginConfiguration(); session.sessionPreset = .hd1920x1080
            let input = try AVCaptureDeviceInput(device: device)
            guard session.canAddInput(input) else { throw NSError(domain: "CameraProbe.Input", code: 1) }
            session.addInput(input)
            let output = AVCaptureVideoDataOutput()
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
            output.alwaysDiscardsLateVideoFrames = true
            let receiver = Receiver(destination: destination, checkBars: !args.contains("--no-bars"), allowStatic: args.contains("--allow-static"), warmup: max(0, min(120, Int(option("--warmup", "0")) ?? 0)))
            output.setSampleBufferDelegate(receiver, queue: receiver.queue)
            guard session.canAddOutput(output) else { throw NSError(domain: "CameraProbe.Output", code: 1) }
            session.addOutput(output); session.commitConfiguration(); session.startRunning()
            let deadline = Date().addingTimeInterval(20)
            while receiver.count < minimum && Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
            session.stopRunning()
            let result = receiver.result(device: device, minimum: minimum)
            let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: destination.appendingPathComponent("capture.json"))
            print(String(decoding: data, as: UTF8.self))
            exit(result["passed"] as? Bool == true ? 0 : 1)
        } catch {
            let result: [String: Any] = ["passed": false, "error": String(describing: error)]
            if let data = try? JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys]) {
                try? data.write(to: destination.appendingPathComponent("capture.json"))
                print(String(decoding: data, as: UTF8.self))
            }
            exit(1)
        }
    }
}
