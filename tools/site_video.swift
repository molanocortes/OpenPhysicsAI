// site_video.swift - turn an animated GIF or WebP into a small H.264 MP4 for the website (site/).
//
//   swift tools/site_video.swift IN OUT.mp4 [KBPS]
//
// Every frame is drawn over the films' dark background (#0d1016), so a transparent pixel in the source never turns
// white or black in the video, and each frame keeps the duration the source gives it. KBPS is the average bit rate
// (default 500). The output carries no audio and has its index at the front, so a browser starts playing before the
// whole file has arrived. macOS only (AVFoundation); the videos are committed, so the site builds anywhere without it.
import AVFoundation
import CoreGraphics
import Foundation
import ImageIO

func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}

let args = CommandLine.arguments
guard args.count >= 3 else { fail("usage: swift tools/site_video.swift IN OUT.mp4 [KBPS]") }
let kbps = args.count > 3 ? (Int(args[3]) ?? 500) : 500
guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil) else { fail("cannot read \(args[1])") }
let count = CGImageSourceGetCount(source)
guard count > 0, let first = CGImageSourceCreateImageAtIndex(source, 0, nil) else { fail("no frame in \(args[1])") }
let width = first.width - first.width % 2, height = first.height - first.height % 2  // H.264 wants even sizes

/// The display time of frame i in seconds, from the GIF or WebP properties; 0.1 s when the source gives none.
func duration(_ i: Int) -> Double {
    let props = CGImageSourceCopyPropertiesAtIndex(source, i, nil) as? [CFString: Any] ?? [:]
    let gif = props[kCGImagePropertyGIFDictionary] as? [CFString: Any]
    let webp = props[kCGImagePropertyWebPDictionary] as? [CFString: Any]
    let candidates: [Any?] = [gif?[kCGImagePropertyGIFUnclampedDelayTime], gif?[kCGImagePropertyGIFDelayTime],
                              webp?[kCGImagePropertyWebPUnclampedDelayTime], webp?[kCGImagePropertyWebPDelayTime]]
    for case let value as Double in candidates where value >= 0.02 { return value }
    return 0.1
}

let output = URL(fileURLWithPath: args[2])
try? FileManager.default.removeItem(at: output)
let writer: AVAssetWriter
do { writer = try AVAssetWriter(outputURL: output, fileType: .mp4) } catch { fail("cannot write \(args[2]): \(error)") }
writer.shouldOptimizeForNetworkUse = true
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264,
    AVVideoWidthKey: width,
    AVVideoHeightKey: height,
    AVVideoCompressionPropertiesKey: [
        AVVideoAverageBitRateKey: kbps * 1000,
        AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
        AVVideoMaxKeyFrameIntervalKey: 48,
    ],
])
input.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
    kCVPixelBufferWidthKey as String: width,
    kCVPixelBufferHeightKey as String: height,
])
writer.add(input)
guard writer.startWriting() else { fail("cannot start writing: \(String(describing: writer.error))") }
writer.startSession(atSourceTime: .zero)

let timescale: CMTimeScale = 600
var t = 0.0
for i in 0..<count {
    guard let image = CGImageSourceCreateImageAtIndex(source, i, nil), let pool = adaptor.pixelBufferPool else { continue }
    var buffer: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
    guard let pixels = buffer else { fail("no pixel buffer") }
    CVPixelBufferLockBaseAddress(pixels, [])
    let context = CGContext(data: CVPixelBufferGetBaseAddress(pixels), width: width, height: height, bitsPerComponent: 8,
                            bytesPerRow: CVPixelBufferGetBytesPerRow(pixels), space: CGColorSpaceCreateDeviceRGB(),
                            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    context.setFillColor(red: 13 / 255, green: 16 / 255, blue: 22 / 255, alpha: 1)
    context.fill(CGRect(x: 0, y: 0, width: width, height: height))
    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    CVPixelBufferUnlockBaseAddress(pixels, [])
    while !input.isReadyForMoreMediaData { usleep(2000) }
    adaptor.append(pixels, withPresentationTime: CMTime(seconds: t, preferredTimescale: timescale))
    t += duration(i)
}
input.markAsFinished()
writer.endSession(atSourceTime: CMTime(seconds: t, preferredTimescale: timescale))
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
guard writer.status == .completed else { fail("writing failed: \(String(describing: writer.error))") }
// AVAssetWriter can leave its first-pass file (OUT.mp4.sb-...) beside the network-optimised one; it is a duplicate
let folder = output.deletingLastPathComponent(), leftover = output.lastPathComponent + ".sb-"
for name in (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? [] where name.hasPrefix(leftover) {
    try? FileManager.default.removeItem(at: folder.appendingPathComponent(name))
}
let bytes = (try? FileManager.default.attributesOfItem(atPath: args[2])[.size] as? Int) ?? 0
print(String(format: "%@: %d frames, %.1f s, %dx%d, %.0f kB", args[2], count, t, width, height, Double(bytes) / 1000))
