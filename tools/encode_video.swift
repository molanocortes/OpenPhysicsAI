// Encode native PNG captures in the supplied order; no resizing or invented frames.
// macOS 13+: swift tools/encode_video.swift output.mp4 fps frame1.png ...
import Foundation
import AVFoundation
import CoreVideo
import ImageIO

struct EncodeError: Error, CustomStringConvertible {
    let description: String
    init(_ message: String) { description = message }
}

func require(_ condition: Bool, _ message: String) throws {
    if !condition { throw EncodeError(message) }
}

func imageSource(_ path: String) throws -> CGImageSource {
    guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil) else {
        throw EncodeError("Cannot read \(path)")
    }
    return source
}

func dimensions(_ path: String) throws -> (Int, Int) {
    let source = try imageSource(path)
    guard let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
          let width = properties[kCGImagePropertyPixelWidth] as? Int,
          let height = properties[kCGImagePropertyPixelHeight] as? Int else {
        throw EncodeError("Cannot read image dimensions: \(path)")
    }
    return (width, height)
}

func encode(_ output: URL, fps: Double, files: [String]) throws -> (Int, Int, CMTime) {
    let (width, height) = try dimensions(files[0])
    try require(width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0,
                "H.264 needs positive even dimensions; capture even-sized frames (the encoder never resizes).")
    for file in files {
        let size = try dimensions(file)
        try require(size.0 == width && size.1 == height, "Frame dimensions differ: \(file)")
    }
    try require(!FileManager.default.fileExists(atPath: output.path), "Output already exists: \(output.path)")
    // The rational frame interval is shared by every timestamp and the final duration.
    // Common integer rates and 29.97/59.94 rates are represented to sub-microsecond accuracy.
    let frameDuration = CMTime(seconds: 1.0 / fps, preferredTimescale: 600_000)
    try require(frameDuration.value > 0, "Frame rate is too high")
    let writer = try AVAssetWriter(outputURL: output, fileType: .mp4)
    writer.movieTimeScale = frameDuration.timescale
    var completed = false
    defer {
        if !completed {
            writer.cancelWriting()
            try? FileManager.default.removeItem(at: output)
        }
    }
    let settings: [String: Any] = [
        AVVideoCodecKey: AVVideoCodecType.h264,
        AVVideoWidthKey: width,
        AVVideoHeightKey: height,
        AVVideoColorPropertiesKey: [
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
        ],
        AVVideoCompressionPropertiesKey: [
            AVVideoAverageBitRateKey: Int(max(8_000_000, Double(width) * Double(height) * fps * 0.4)),
            AVVideoExpectedSourceFrameRateKey: fps,
            AVVideoAllowFrameReorderingKey: false,
            AVVideoMaxKeyFrameIntervalKey: max(1, Int(fps.rounded())),
            AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
        ],
    ]
    try require(writer.canApply(outputSettings: settings, forMediaType: .video), "H.264 settings are unsupported")
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
    input.expectsMediaDataInRealTime = false
    input.mediaTimeScale = frameDuration.timescale
    let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        kCVPixelBufferWidthKey as String: width,
        kCVPixelBufferHeightKey as String: height,
        kCVPixelBufferCGImageCompatibilityKey as String: true,
        kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
        kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ])
    try require(writer.canAdd(input), "Cannot add the video input")
    writer.add(input)
    try require(writer.startWriting(), "Cannot start encoding: \(String(describing: writer.error))")
    writer.startSession(atSourceTime: .zero)
    guard let pool = adaptor.pixelBufferPool,
          let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
        throw EncodeError("Cannot create the pixel-buffer pool or sRGB color space")
    }
    for (index, file) in files.enumerated() {
        let deadline = Date().addingTimeInterval(60)
        while !input.isReadyForMoreMediaData {
            try require(writer.status == .writing, "Encoder failed: \(String(describing: writer.error))")
            try require(Date() < deadline, "Timed out waiting for the encoder")
            Thread.sleep(forTimeInterval: 0.005)
        }
        try autoreleasepool {
            let source = try imageSource(file)
            guard let image = CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary) else {
                throw EncodeError("Cannot decode \(file)")
            }
            var storage: CVPixelBuffer?
            let status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &storage)
            guard status == kCVReturnSuccess, let buffer = storage else {
                throw EncodeError("Cannot allocate a pixel buffer: \(status)")
            }
            try require(CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess, "Cannot lock pixel buffer")
            defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
            guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height,
                                          bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
                                          space: colorSpace,
                                          bitmapInfo: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue) else {
                throw EncodeError("Cannot create the video bitmap context")
            }
            // This direct, same-size draw performs only the required pixel/color conversion.
            context.interpolationQuality = .none
            context.setBlendMode(.copy)
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            CVBufferSetAttachment(buffer, kCVImageBufferCGColorSpaceKey, colorSpace, .shouldPropagate)
            let timestamp = CMTimeMultiply(frameDuration, multiplier: Int32(index))
            try require(adaptor.append(buffer, withPresentationTime: timestamp),
                        "Cannot append frame \(index): \(String(describing: writer.error))")
        }
    }
    writer.endSession(atSourceTime: CMTimeMultiply(frameDuration, multiplier: Int32(files.count)))
    input.markAsFinished()
    let finished = DispatchSemaphore(value: 0)
    writer.finishWriting { finished.signal() }
    try require(finished.wait(timeout: .now() + 60) == .success, "Timed out finishing the video")
    try require(writer.status == .completed, "Cannot finish video: \(String(describing: writer.error))")
    completed = true
    return (width, height, frameDuration)
}

// Inspect the written stream itself: exact dimensions, one sample per supplied
// frame, and the expected presentation time for every frame. No decode or edit.
func verify(_ url: URL, width: Int, height: Int, count: Int, interval: CMTime) async throws {
    let asset = AVURLAsset(url: url)
    let tracks = try await asset.loadTracks(withMediaType: .video)
    try require(tracks.count == 1, "Output does not contain exactly one video track")
    let track = tracks[0]
    let size = try await track.load(.naturalSize)
    try require(size.width == CGFloat(width) && size.height == CGFloat(height), "Encoder changed frame dimensions")
    let reader = try AVAssetReader(asset: asset)
    let output = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
    output.alwaysCopiesSampleData = false
    try require(reader.canAdd(output), "Cannot inspect the output video")
    reader.add(output)
    try require(reader.startReading(), "Cannot read the output video")
    var index = 0
    while let sample = output.copyNextSampleBuffer() {
        // AVAssetReader may group consecutive compressed frames in one buffer.
        for sampleIndex in 0..<CMSampleBufferGetNumSamples(sample) {
            var timing = CMSampleTimingInfo()
            try require(CMSampleBufferGetSampleTimingInfo(sample, at: sampleIndex, timingInfoOut: &timing) == noErr,
                        "Cannot inspect frame timing")
            let expected = CMTimeMultiply(interval, multiplier: Int32(index))
            try require(CMTimeCompare(timing.presentationTimeStamp, expected) == 0,
                        "Missing, duplicated or reordered timestamp at frame \(index)")
            index += 1
        }
        if let format = CMSampleBufferGetFormatDescription(sample) {
            try require(CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_H264, "Unexpected output codec")
        }
    }
    try require(reader.status == .completed, "Output verification failed: \(String(describing: reader.error))")
    try require(index == count, "Output has \(index) frames; expected \(count)")
    let duration = try await track.load(.timeRange).duration
    try require(CMTimeCompare(duration, CMTimeMultiply(interval, multiplier: Int32(count))) == 0,
                "Output duration differs from the supplied frame count")
}

let arguments = CommandLine.arguments
do {
    try require(arguments.count >= 4, "usage: encode_video.swift output.mp4 fps frames...")
    guard let fps = Double(arguments[2]), fps.isFinite, fps > 0, fps <= 240 else {
        throw EncodeError("fps must be finite, greater than zero and at most 240")
    }
    let files = Array(arguments.dropFirst(3))
    try require(files.count <= Int(Int32.max), "Too many frames")
    let output = URL(fileURLWithPath: arguments[1])
    let (width, height, interval) = try encode(output, fps: fps, files: files)
    try await verify(output, width: width, height: height, count: files.count, interval: interval)
    print("Encoded and verified \(files.count) frames at \(fps) fps, \(width)x\(height): \(output.path)")
} catch {
    FileHandle.standardError.write(Data("encode_video: \(error)\n".utf8))
    exit(1)
}
