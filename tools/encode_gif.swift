// Encode native PNG captures without retouching or inventing simulation frames.
// swift tools/encode_gif.swift output.gif delay_seconds frame1.png ...
import Foundation
import ImageIO
let args = CommandLine.arguments
 guard args.count >= 4, let delay = Double(args[2]), delay > 0 else {
    fatalError("usage: encode_gif.swift output.gif delay_seconds frames...")
}
let files = Array(args.dropFirst(3))
guard let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL,
    "com.compuserve.gif" as CFString, files.count, nil) else { fatalError("Cannot create GIF") }
CGImageDestinationSetProperties(destination, [kCGImagePropertyGIFDictionary:
    [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
for file in files {
    guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: file) as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { fatalError("Cannot read \(file)") }
    CGImageDestinationAddImage(destination, image, [kCGImagePropertyGIFDictionary:
        [kCGImagePropertyGIFDelayTime: delay]] as CFDictionary)
}
guard CGImageDestinationFinalize(destination) else { fatalError("Cannot finish GIF") }
print("Encoded \(files.count) frames: \(args[1])")
