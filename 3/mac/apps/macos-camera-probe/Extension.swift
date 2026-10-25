import Foundation
import CoreMediaIO
import IOKit.audio
import os

private let logger = Logger(subsystem: "com.pondashicom.vtrpon.CameraProbe.Extension", category: "camera")

final class ProbeDevice: NSObject, CMIOExtensionDeviceSource {
    private(set) var device: CMIOExtensionDevice!
    private var source: ProbeStream!
    private var pool: CVPixelBufferPool!
    private var videoDescription: CMVideoFormatDescription!
    private var timer: DispatchSourceTimer?
    private var users = 0
    private var frame: UInt64 = 0
    let queue: DispatchQueue

    init(queue: DispatchQueue) throws {
        self.queue = queue
        super.init()
        device = CMIOExtensionDevice(localizedName: TestPattern.name, deviceID: TestPattern.deviceID, legacyDeviceID: nil, source: self)
        let attrs: [String: Any] = [
            kCVPixelBufferWidthKey as String: TestPattern.width,
            kCVPixelBufferHeightKey as String: TestPattern.height,
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ]
        let status = CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        guard status == kCVReturnSuccess else { throw NSError(domain: NSOSStatusErrorDomain, code: Int(status)) }
        let formatStatus = CMVideoFormatDescriptionCreate(allocator: nil, codecType: kCVPixelFormatType_32BGRA,
            width: Int32(TestPattern.width), height: Int32(TestPattern.height), extensions: nil, formatDescriptionOut: &videoDescription)
        guard formatStatus == noErr else { throw NSError(domain: NSOSStatusErrorDomain, code: Int(formatStatus)) }
        let duration = CMTime(value: 1, timescale: TestPattern.fps)
        let format = CMIOExtensionStreamFormat(formatDescription: videoDescription, maxFrameDuration: duration, minFrameDuration: duration, validFrameDurations: nil)
        source = ProbeStream(device: self, format: format)
        try device.addStream(source.stream)
    }

    var availableProperties: Set<CMIOExtensionProperty> { [.deviceTransportType, .deviceModel] }
    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
        let result = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) { result.transportType = kIOAudioDeviceTransportTypeVirtual }
        if properties.contains(.deviceModel) { result.model = "VTR-PON isolated camera prototype" }
        return result
    }
    func setDeviceProperties(_ properties: CMIOExtensionDeviceProperties) throws {}

    func start() {
        users += 1
        guard users == 1 else { return }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .nanoseconds(Int(1_000_000_000 / TestPattern.fps)), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.sendFrame() }
        self.timer = timer
        timer.resume()
        logger.notice("Test stream started")
    }
    func stop() {
        users = max(0, users - 1)
        if users == 0 { timer?.cancel(); timer = nil; logger.notice("Test stream stopped") }
    }
    private func sendFrame() {
        autoreleasepool {
            var buffer: CVPixelBuffer?
            // Bound memory even if a client retains samples. Drop rather than wait.
            let aux = [kCVPixelBufferPoolAllocationThresholdKey as String: 6] as CFDictionary
            guard CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nil, pool, aux, &buffer) == kCVReturnSuccess,
                  let buffer else { return }
            do { try TestPattern.draw(buffer, frame: frame) }
            catch { logger.error("Pattern drawing failed: \(error.localizedDescription)"); return }
            let now = CMClockGetTime(CMClockGetHostTimeClock())
            var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: TestPattern.fps), presentationTimeStamp: now, decodeTimeStamp: .invalid)
            var sample: CMSampleBuffer?
            guard CMSampleBufferCreateForImageBuffer(allocator: nil, imageBuffer: buffer, dataReady: true,
                makeDataReadyCallback: nil, refcon: nil, formatDescription: videoDescription,
                sampleTiming: &timing, sampleBufferOut: &sample) == noErr, let sample else { return }
            let nanoseconds = CMTimeConvertScale(now, timescale: 1_000_000_000, method: .default).value
            source.stream.send(sample, discontinuity: [], hostTimeInNanoseconds: UInt64(max(0, nanoseconds)))
            frame &+= 1
        }
    }
}

final class ProbeStream: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!
    private unowned let owner: ProbeDevice
    let formats: [CMIOExtensionStreamFormat]
    init(device: ProbeDevice, format: CMIOExtensionStreamFormat) {
        owner = device
        formats = [format]
        super.init()
        stream = CMIOExtensionStream(localizedName: "VTR-PON Camera Test Video", streamID: TestPattern.streamID, direction: .source, clockType: .hostTime, source: self)
    }
    var availableProperties: Set<CMIOExtensionProperty> { [.streamActiveFormatIndex, .streamFrameDuration] }
    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { result.activeFormatIndex = 0 }
        if properties.contains(.streamFrameDuration) { result.frameDuration = CMTime(value: 1, timescale: TestPattern.fps) }
        return result
    }
    func setStreamProperties(_ properties: CMIOExtensionStreamProperties) throws {
        if let index = properties.activeFormatIndex, index != 0 { throw NSError(domain: "CameraProbe.Format", code: 1) }
        if let duration = properties.frameDuration, CMTimeCompare(duration, CMTime(value: 1, timescale: TestPattern.fps)) != 0 {
            throw NSError(domain: "CameraProbe.FrameDuration", code: 1)
        }
    }
    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }
    func startStream() throws { owner.start() }
    func stopStream() throws { owner.stop() }
}

final class ProbeProvider: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!
    private var device: ProbeDevice!
    override init() {
        super.init()
        // All stream callbacks and timer events share one serial queue.
        let queue = DispatchQueue(label: "com.pondashicom.vtrpon.CameraProbe.stream", qos: .userInitiated)
        provider = CMIOExtensionProvider(source: self, clientQueue: queue)
        do { device = try ProbeDevice(queue: queue); try provider.addDevice(device.device) }
        catch { fatalError("Camera initialization failed: \(error)") }
    }
    func connect(to client: CMIOExtensionClient) throws {}
    func disconnect(from client: CMIOExtensionClient) {}
    var availableProperties: Set<CMIOExtensionProperty> { [.providerManufacturer] }
    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionProviderProperties {
        let result = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerManufacturer) { result.manufacturer = "VTR-PON" }
        return result
    }
    func setProviderProperties(_ properties: CMIOExtensionProviderProperties) throws {}
}

@main enum ExtensionMain {
    static func main() {
        let source = ProbeProvider()
        CMIOExtensionProvider.startService(provider: source.provider)
        withExtendedLifetime(source) { CFRunLoopRun() }
    }
}
