// montage.swift - one picture from frames of several films: tiles in a grid, each with its caption.
//
//   swift tools/showcase/montage.swift OUT.png COLUMNS TILE_W TILE_H  "caption|image[|frame]" ...
//
// An image may be a PNG or a GIF (the frame index picks one frame of a GIF; default: the middle one). Tiles keep the
// aspect ratio of their image (fit, dark border). Nothing is retouched: the tiles are the solvers' own frames.
import AppKit
import CoreText
import Foundation
import ImageIO

let a = CommandLine.arguments
guard a.count >= 6, let cols = Int(a[2]), let tw = Int(a[3]), let th = Int(a[4]) else {
    fatalError("usage: montage.swift OUT.png COLUMNS TILE_W TILE_H \"caption|image[|frame]\" ...")
}
let items = Array(a.dropFirst(5))
let rows = (items.count + cols - 1) / cols
let gap = 6
let W = cols * tw + (cols + 1) * gap, H = rows * th + (rows + 1) * gap
let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.setFillColor(CGColor(red: 0.055, green: 0.067, blue: 0.10, alpha: 1))
ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
for (k, item) in items.enumerated() {
    let parts = item.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
    guard parts.count >= 2, let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: parts[1]) as CFURL, nil) else {
        fatalError("cannot read \(item)")
    }
    let n = CGImageSourceGetCount(src)
    let idx = parts.count >= 3 ? min(Int(parts[2]) ?? n / 2, n - 1) : n / 2
    guard let img = CGImageSourceCreateImageAtIndex(src, idx, nil) else { fatalError("no frame \(idx) in \(parts[1])") }
    let r = k / cols, c = k % cols
    let x0 = gap + c * (tw + gap), y0 = H - (gap + (r + 1) * th + r * gap)
    ctx.setFillColor(CGColor(red: 0.09, green: 0.11, blue: 0.15, alpha: 1))
    ctx.fill(CGRect(x: x0, y: y0, width: tw, height: th))
    let s = min(Double(tw) / Double(img.width), Double(th) / Double(img.height))
    let dw = Double(img.width) * s, dh = Double(img.height) * s
    ctx.interpolationQuality = .high
    ctx.draw(img, in: CGRect(x: Double(x0) + (Double(tw) - dw) / 2, y: Double(y0) + (Double(th) - dh) / 2, width: dw, height: dh))
    // the caption on a dark band at the bottom of the tile
    let band = 30.0
    ctx.setFillColor(CGColor(red: 0.04, green: 0.05, blue: 0.08, alpha: 0.82))
    ctx.fill(CGRect(x: Double(x0), y: Double(y0), width: Double(tw), height: band))
    let font = CTFontCreateWithName("Helvetica-Bold" as CFString, 15, nil)
    let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: CGColor(red: 0.93, green: 0.94, blue: 0.97, alpha: 1)]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: parts[0], attributes: attrs))
    ctx.textPosition = CGPoint(x: Double(x0) + 10, y: Double(y0) + 10)
    CTLineDraw(line, ctx)
}
let out = NSBitmapImageRep(cgImage: ctx.makeImage()!)
try! out.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[1]))
print("wrote \(a[1]) (\(W) x \(H))")
