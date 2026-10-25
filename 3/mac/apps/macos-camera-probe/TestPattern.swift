import CoreVideo
import CoreGraphics
import CoreText
import Foundation

// Shared by the extension and the offline pattern check. No camera or microphone input.
enum TestPattern {
    static let width = 1920
    static let height = 1080
    static let fps: Int32 = 30
    static let deviceID = UUID(uuidString: "64CB4218-4EC4-41DB-A5A1-63D4B164EF56")!
    static let streamID = UUID(uuidString: "F74E7C58-96B8-40C2-AD98-1028A9427948")!
    static let name = "VTR-PON Camera Test"
    static let colors: [(CGFloat, CGFloat, CGFloat)] = [
        (1,1,1), (1,1,0), (0,1,1), (0,1,0), (1,0,1), (1,0,0), (0,0,1), (0,0,0)
    ]

    static func draw(_ buffer: CVPixelBuffer, frame: UInt64) throws {
        guard CVPixelBufferGetWidth(buffer) == width,
              CVPixelBufferGetHeight(buffer) == height,
              CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA,
              CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess else {
            throw NSError(domain: "CameraProbe.Pattern", code: 1)
        }
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let base = CVPixelBufferGetBaseAddress(buffer),
              let context = CGContext(data: base, width: width, height: height,
                  bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
                  space: CGColorSpaceCreateDeviceRGB(),
                  bitmapInfo: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue) else {
            throw NSError(domain: "CameraProbe.Pattern", code: 2)
        }
        for (index, color) in colors.enumerated() {
            context.setFillColor(red: color.0, green: color.1, blue: color.2, alpha: 1)
            context.fill(CGRect(x: index * width / 8, y: 0, width: width / 8, height: height))
        }
        context.setFillColor(gray: 0.08, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: width, height: 180))
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: Int(frame % 240) * (width - 80) / 239, y: 230, width: 80, height: 80))
        let title = "VTR-PON CAMERA TEST  |  1920 x 1080  |  30 fps"
        let counter = String(format: "FRAME %08llu   TIME %02llu:%02llu:%02llu", frame, frame / 108000, frame / 1800 % 60, frame / 30 % 60)
        for (text, y, size) in [(title, 110.0, 42.0), (counter, 45.0, 36.0)] {
            let attributes: [NSAttributedString.Key: Any] = [
                NSAttributedString.Key(kCTFontAttributeName as String): CTFontCreateWithName("Menlo" as CFString, size, nil),
                NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1)
            ]
            context.textPosition = CGPoint(x: 60, y: y)
            CTLineDraw(CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes)), context)
        }
    }
}
