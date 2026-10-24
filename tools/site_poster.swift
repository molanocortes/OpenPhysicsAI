// site_poster.swift - write one frame of an animated GIF or WebP as a JPEG poster for the website (site/).
//
//   swift tools/site_poster.swift IN OUT.jpg [FRACTION] [QUALITY]
//
// FRACTION picks the frame by its place in the animation (0 = first, 0.5 = middle; default 0.5), because the first
// frame of a simulation film is often the empty state before anything has happened. QUALITY is the JPEG quality from
// 0 to 1 (default 0.74). macOS only (ImageIO); the posters are committed, so the site builds anywhere without it.
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: swift tools/site_poster.swift IN OUT.jpg [FRACTION] [QUALITY]\n".data(using: .utf8)!)
    exit(2)
}
let fraction = args.count > 3 ? max(0, min(1, Double(args[3]) ?? 0.5)) : 0.5
let quality = args.count > 4 ? max(0, min(1, Double(args[4]) ?? 0.74)) : 0.74

guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil) else {
    FileHandle.standardError.write("cannot read \(args[1])\n".data(using: .utf8)!)
    exit(1)
}
let count = CGImageSourceGetCount(src)
let index = min(count - 1, Int((Double(count - 1) * fraction).rounded()))
guard count > 0, let frame = CGImageSourceCreateImageAtIndex(src, index, nil) else {
    FileHandle.standardError.write("no frame in \(args[1])\n".data(using: .utf8)!)
    exit(1)
}
guard let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[2]) as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else {
    FileHandle.standardError.write("cannot write \(args[2])\n".data(using: .utf8)!)
    exit(1)
}
CGImageDestinationAddImage(dst, frame, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
guard CGImageDestinationFinalize(dst) else {
    FileHandle.standardError.write("cannot finish \(args[2])\n".data(using: .utf8)!)
    exit(1)
}
print("\(args[2]): frame \(index + 1) of \(count), \(frame.width)x\(frame.height)")
