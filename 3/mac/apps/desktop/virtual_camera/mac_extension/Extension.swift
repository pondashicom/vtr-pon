// SPDX-License-Identifier: MPL-2.0
import Foundation
import CoreMediaIO
import CoreVideo
import CoreGraphics
import CoreText
import IOKit.audio
import os

private let log = Logger(subsystem: "com.pondashi.vtrpon3.camera", category: "extension")
private let width = 1920, height = 1080
private let duration = CMTime(value: 1, timescale: 30)
private let usersProperty = CMIOExtensionProperty(rawValue: "4cc_vcus_glob_0000")
private let deviceID = UUID(uuidString: "BAC4B305-4C38-44BE-A1D3-DA855F14415D")!
private let sourceID = UUID(uuidString: "A45554B6-60F9-43F6-A15D-CA1549751510")!
private let sinkID = UUID(uuidString: "E734F43F-56DB-4615-8264-784E46AA1153")!
private func nowNs() -> UInt64 {
    UInt64(max(0, CMTimeConvertScale(CMClockGetTime(CMClockGetHostTimeClock()), timescale: 1_000_000_000, method: .default).value))
}

final class CameraDevice: NSObject, CMIOExtensionDeviceSource {
    private(set) var device: CMIOExtensionDevice!
    private var output: CameraStream!
    private var input: CameraStream!
    private var sourceTimer: DispatchSourceTimer?, sinkTimer: DispatchSourceTimer?
    private var sourceUsers = 0
    private var producer: CMIOExtensionClient?
    private var generation: UInt64 = 0
    private var consuming = false
    private var latest: CVPixelBuffer?
    private var receivedAt: UInt64 = 0
    private var lastInputTime = CMTime.invalid
    private var descriptionVideo: CMVideoFormatDescription!
    private var waiting: CVPixelBuffer!, black: CVPixelBuffer!
    let queue: DispatchQueue

    init(queue: DispatchQueue) throws {
        self.queue = queue
        super.init()
        device = CMIOExtensionDevice(localizedName: "VTR-PON Virtual Camera", deviceID: deviceID, legacyDeviceID: nil, source: self)
        let status = CMVideoFormatDescriptionCreate(allocator: nil, codecType: kCVPixelFormatType_32BGRA, width: Int32(width), height: Int32(height), extensions: nil, formatDescriptionOut: &descriptionVideo)
        guard status == noErr else { throw NSError(domain: NSOSStatusErrorDomain, code: Int(status)) }
        let format = CMIOExtensionStreamFormat(formatDescription: descriptionVideo, maxFrameDuration: duration, minFrameDuration: duration, validFrameDurations: nil)
        output = CameraStream(owner: self, format: format, sink: false)
        input = CameraStream(owner: self, format: format, sink: true)
        try device.addStream(output.stream); try device.addStream(input.stream)
        waiting = try makeFallback(label: true); black = try makeFallback(label: false)
    }
    func makeFallback(label: Bool) throws -> CVPixelBuffer {
        var value: CVPixelBuffer?
        let attrs = [kCVPixelBufferIOSurfacePropertiesKey as String: [:]] as CFDictionary
        guard CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_32BGRA, attrs, &value) == kCVReturnSuccess, let value else {
            throw NSError(domain: "VTRCamera.Buffer", code: 1)
        }
        CVPixelBufferLockBaseAddress(value, [])
        defer { CVPixelBufferUnlockBaseAddress(value, []) }
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(value), width: width, height: height, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(value), space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue) else {
            throw NSError(domain: "VTRCamera.Buffer", code: 2)
        }
        context.setFillColor(gray: 0, alpha: 1); context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        if label {
            let attrs: [NSAttributedString.Key: Any] = [NSAttributedString.Key(kCTFontAttributeName as String): CTFontCreateWithName("Menlo" as CFString, 28, nil), NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1)]
            context.textPosition = CGPoint(x: 60, y: height / 2)
            CTLineDraw(CTLineCreateWithAttributedString(NSAttributedString(string: "VTR-PON Virtual Camera", attributes: attrs)), context)
        }
        return value
    }
    var availableProperties: Set<CMIOExtensionProperty> { [.deviceTransportType, .deviceModel, usersProperty] }
    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
        var custom: [CMIOExtensionProperty: CMIOExtensionPropertyState<AnyObject>] = [:]
        if properties.contains(usersProperty) { custom[usersProperty] = CMIOExtensionPropertyState(value: String(sourceUsers) as NSString) }
        let result = CMIOExtensionDeviceProperties(dictionary: custom)
        if properties.contains(.deviceTransportType) { result.transportType = kIOAudioDeviceTransportTypeVirtual }
        if properties.contains(.deviceModel) { result.model = "VTR-PON ON AIR camera" }
        return result
    }
    func setDeviceProperties(_ properties: CMIOExtensionDeviceProperties) throws {}
    func authorize(_ client: CMIOExtensionClient) -> Bool {
        // macOS can withhold the signing identifier ("unknown"). CMIO controls
        // access to this sink; enforce one producer, and reject known other IDs.
        // The host separately verifies the helper's signature and team.
        guard producer == nil || producer?.clientID == client.clientID else { return false }
        if let id = client.signingID, id != "unknown", id != "com.pondashi.vtrpon3.camera-output" {
            log.error("Producer identity rejected signingID=\(id, privacy: .public)")
            return false
        }
        producer = client
        return true
    }
    func startSource() {
        sourceUsers += 1
        device.notifyPropertiesChanged([usersProperty: CMIOExtensionPropertyState<AnyObject>(value: String(sourceUsers) as NSString)])
        guard sourceUsers == 1 else { return }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / 30), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.send() }
        sourceTimer = timer; timer.resume(); log.notice("Camera consumer started")
    }
    func stopSource() {
        sourceUsers = max(0, sourceUsers - 1)
        device.notifyPropertiesChanged([usersProperty: CMIOExtensionPropertyState<AnyObject>(value: String(sourceUsers) as NSString)])
        if sourceUsers == 0 { sourceTimer?.cancel(); sourceTimer = nil; log.notice("Camera consumer stopped") }
    }
    func startSink() throws {
        guard producer != nil else { throw NSError(domain: "VTRCamera.Producer", code: 1) }
        generation &+= 1; latest = nil; receivedAt = 0; lastInputTime = .invalid
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / 60), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.consume() }
        sinkTimer?.cancel(); sinkTimer = timer; timer.resume(); log.notice("ON AIR producer connected")
    }
    func stopSink() {
        generation &+= 1; sinkTimer?.cancel(); sinkTimer = nil
        consuming = false; producer = nil; latest = nil; receivedAt = 0; lastInputTime = .invalid
        log.notice("ON AIR producer disconnected")
    }
    func disconnected(_ client: CMIOExtensionClient) { if producer?.clientID == client.clientID { stopSink() } }
    private func consume() {
        guard !consuming, let client = producer, sinkTimer != nil else { return }
        consuming = true
        let epoch = generation
        input.stream.consumeSampleBuffer(from: client) { [weak self] sample, sequence, _, _, error in
            guard let self else { return }
            self.queue.async {
                guard epoch == self.generation else { return }
                self.consuming = false
                let now = nowNs()
                if error == nil, let sample, let pixels = CMSampleBufferGetImageBuffer(sample) {
                    let pts = CMSampleBufferGetPresentationTimeStamp(sample)
                    let age = CMTimeGetSeconds(CMTimeSubtract(CMTime(value: Int64(now), timescale: 1_000_000_000), pts))
                    if CVPixelBufferGetWidth(pixels) == width && CVPixelBufferGetHeight(pixels) == height && CVPixelBufferGetPixelFormatType(pixels) == kCVPixelFormatType_32BGRA && pts.isValid && age >= 0 && age < 1.5 && (!self.lastInputTime.isValid || CMTimeCompare(pts, self.lastInputTime) >= 0) {
                        self.latest = pixels; self.receivedAt = now; self.lastInputTime = pts
                    }
                }
                self.input.stream.notifyScheduledOutputChanged(CMIOExtensionScheduledOutput(sequenceNumber: sequence, hostTimeInNanoseconds: now))
            }
        }
    }
    private func send() {
        let now = nowNs()
        let pixels: CVPixelBuffer
        if let frame = latest, receivedAt > 0 && now - receivedAt < 1_500_000_000 { pixels = frame }
        else { pixels = producer == nil ? waiting : black }
        var timing = CMSampleTimingInfo(duration: duration, presentationTimeStamp: CMTime(value: Int64(now), timescale: 1_000_000_000), decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        if CMSampleBufferCreateForImageBuffer(allocator: nil, imageBuffer: pixels, dataReady: true, makeDataReadyCallback: nil, refcon: nil, formatDescription: descriptionVideo, sampleTiming: &timing, sampleBufferOut: &sample) == noErr, let sample {
            output.stream.send(sample, discontinuity: [], hostTimeInNanoseconds: now)
        }
    }
}

final class CameraStream: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!
    private unowned let owner: CameraDevice
    private let sink: Bool
    let formats: [CMIOExtensionStreamFormat]
    init(owner: CameraDevice, format: CMIOExtensionStreamFormat, sink: Bool) {
        self.owner = owner; self.sink = sink; formats = [format]; super.init()
        stream = CMIOExtensionStream(localizedName: sink ? "VTR-PON ON AIR Input" : "VTR-PON Video", streamID: sink ? sinkID : sourceID, direction: sink ? .sink : .source, clockType: .hostTime, source: self)
    }
    var availableProperties: Set<CMIOExtensionProperty> {
        sink ? [.streamActiveFormatIndex, .streamFrameDuration, .streamSinkBufferQueueSize, .streamSinkBuffersRequiredForStartup] : [.streamActiveFormatIndex, .streamFrameDuration]
    }
    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { result.activeFormatIndex = 0 }
        if properties.contains(.streamFrameDuration) { result.frameDuration = duration }
        if properties.contains(.streamSinkBufferQueueSize) { result.sinkBufferQueueSize = 3 }
        if properties.contains(.streamSinkBuffersRequiredForStartup) { result.sinkBuffersRequiredForStartup = 1 }
        return result
    }
    func setStreamProperties(_ properties: CMIOExtensionStreamProperties) throws {
        if let index = properties.activeFormatIndex, index != 0 { throw NSError(domain: "VTRCamera.Format", code: 1) }
        if let time = properties.frameDuration, CMTimeCompare(time, duration) != 0 { throw NSError(domain: "VTRCamera.Duration", code: 1) }
    }
    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { !sink || owner.authorize(client) }
    func startStream() throws { if sink { try owner.startSink() } else { owner.startSource() } }
    func stopStream() throws { if sink { owner.stopSink() } else { owner.stopSource() } }
}
final class CameraProvider: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!
    private var device: CameraDevice!
    override init() {
        super.init()
        let queue = DispatchQueue(label: "com.pondashi.vtrpon3.camera", qos: .userInitiated)
        provider = CMIOExtensionProvider(source: self, clientQueue: queue)
        do { device = try CameraDevice(queue: queue); try provider.addDevice(device.device) }
        catch { fatalError("Camera initialization: \(error)") }
    }
    func connect(to client: CMIOExtensionClient) throws {}
    func disconnect(from client: CMIOExtensionClient) { device.disconnected(client) }
    var availableProperties: Set<CMIOExtensionProperty> { [.providerManufacturer] }
    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionProviderProperties {
        let result = CMIOExtensionProviderProperties(dictionary: [:]); result.manufacturer = "VTR-PON"; return result
    }
    func setProviderProperties(_ properties: CMIOExtensionProviderProperties) throws {}
}
@main enum CameraMain {
    static func main() {
        let source = CameraProvider()
        CMIOExtensionProvider.startService(provider: source.provider)
        withExtendedLifetime(source) { CFRunLoopRun() }
    }
}
