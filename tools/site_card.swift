// site_card.swift - draw the website's share card (1200 x 630, shown when a link to the site is posted) and its
// home-screen icon (180 x 180), in the studio look of docs/app/DESIGN.md: the warm page colour, SF Pro light type, the
// house colour map as the only gradient, and a frame of the lab's own films on a white card.
//
//   swift tools/site_card.swift HERO.jpg CARD.jpg ICON.png "Name" "Headline" "label"
//
// macOS only (AppKit for the system fonts); the outputs are committed, so the site builds anywhere without it.
import AppKit
import Foundation
import ImageIO
import UniformTypeIdentifiers

let a = CommandLine.arguments
guard a.count >= 7 else { fatalError("usage: swift tools/site_card.swift HERO.jpg CARD.jpg ICON.png NAME HEADLINE LABEL") }
let (heroPath, cardPath, iconPath, name, headline, label) = (a[1], a[2], a[3], a[4], a[5], a[6])

func hex(_ v: UInt32, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat(v >> 16 & 255) / 255, green: CGFloat(v >> 8 & 255) / 255, blue: CGFloat(v & 255) / 255, alpha: alpha)
}
let page = hex(0xF2F1EE), ink = hex(0x111111), dim = hex(0x7A7A80), accent = hex(0xE8551C)
let studio: [UInt32] = [0x240c4f, 0x420a68, 0x6a176e, 0x932667, 0xba3655, 0xdd513a, 0xf37819, 0xfca50a, 0xf3cf45]
let studioGradient = NSGradient(colors: studio.map { hex($0) })!

/// Draw into a fresh top-left-origin bitmap of w x h points and return it.
func render(_ w: Int, _ h: Int, _ draw: (CGContext) -> Void) -> CGImage {
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.translateBy(x: 0, y: CGFloat(h))
    ctx.scaleBy(x: 1, y: -1)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: true)
    draw(ctx)
    NSGraphicsContext.restoreGraphicsState()
    return ctx.makeImage()!
}

func save(_ image: CGImage, _ path: String, _ type: UTType, _ quality: CGFloat) {
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, type.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, image, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
    guard CGImageDestinationFinalize(d) else { fatalError("cannot write \(path)") }
}

/// The mark of favicon.svg: a black rounded square, a ring in the house colour map, and the one orange dot.
func mark(_ rect: NSRect, corner: CGFloat) {
    ink.setFill()
    NSBezierPath(roundedRect: rect, xRadius: corner, yRadius: corner).fill()
    let s = rect.width / 64
    // the ring is the gradient clipped to the band between two circles (radius 20.5 and 13.5 of 64, as in the SVG)
    NSGraphicsContext.saveGraphicsState()
    let outline = NSBezierPath(ovalIn: NSRect(x: rect.minX + 11.5 * s, y: rect.minY + 11.5 * s, width: 41 * s, height: 41 * s))
    let hole = NSBezierPath(ovalIn: NSRect(x: rect.minX + 18.5 * s, y: rect.minY + 18.5 * s, width: 27 * s, height: 27 * s))
    outline.append(hole.reversed)
    outline.addClip()
    studioGradient.draw(in: NSRect(x: rect.minX + 11 * s, y: rect.minY + 11 * s, width: 42 * s, height: 42 * s), angle: -45)  // violet at bottom left, gold at top right, as in favicon.svg (y points down here)
    NSGraphicsContext.restoreGraphicsState()
    accent.setFill()
    NSBezierPath(ovalIn: NSRect(x: rect.minX + 40.5 * s, y: rect.minY + 14.5 * s, width: 9 * s, height: 9 * s)).fill()
}

// ---- the share card --------------------------------------------------------------------------------------------
guard let heroSource = CGImageSourceCreateWithURL(URL(fileURLWithPath: heroPath) as CFURL, nil),
      let hero = CGImageSourceCreateImageAtIndex(heroSource, 0, nil) else { fatalError("cannot read \(heroPath)") }
let W = 1200, H = 630
let card = render(W, H) { ctx in
    page.setFill()
    NSRect(x: 0, y: 0, width: W, height: H).fill()

    // the films on a white card with a soft shadow, bleeding off the right edge
    let frame = NSRect(x: 612, y: 92, width: 640, height: 384)
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor(white: 0, alpha: 0.10)
    shadow.shadowBlurRadius = 30
    shadow.shadowOffset = NSSize(width: 0, height: -10)
    shadow.set()
    NSColor.white.setFill()
    NSBezierPath(roundedRect: frame, xRadius: 22, yRadius: 22).fill()
    NSGraphicsContext.restoreGraphicsState()
    let inner = frame.insetBy(dx: 12, dy: 12)
    NSGraphicsContext.saveGraphicsState()
    NSBezierPath(roundedRect: inner, xRadius: 13, yRadius: 13).addClip()
    NSImage(cgImage: hero, size: NSSize(width: hero.width, height: hero.height))
        .draw(in: inner, from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
    NSGraphicsContext.restoreGraphicsState()

    // the name with its mark
    mark(NSRect(x: 64, y: 64, width: 46, height: 46), corner: 11)
    (name as NSString).draw(at: NSPoint(x: 124, y: 70), withAttributes: [
        .font: NSFont.systemFont(ofSize: 30, weight: .semibold), .foregroundColor: ink, .kern: -0.3,
    ])

    // the headline, large and light
    let para = NSMutableParagraphStyle()
    para.lineHeightMultiple = 0.98
    (headline as NSString).draw(in: NSRect(x: 64, y: 156, width: 520, height: 330), withAttributes: [
        .font: NSFont.systemFont(ofSize: 54, weight: .light), .foregroundColor: ink, .kern: -1.2, .paragraphStyle: para,
    ])

    // the label in mono, and the house map as a thin rule along the bottom
    (label as NSString).draw(at: NSPoint(x: 64, y: 538), withAttributes: [
        .font: NSFont.monospacedSystemFont(ofSize: 19, weight: .regular), .foregroundColor: dim,
    ])
    studioGradient.draw(in: NSRect(x: 0, y: H - 10, width: W, height: 10), angle: 0)
}
save(card, cardPath, .jpeg, 0.88)

// ---- the home-screen icon (iOS rounds the corners itself, so the square is drawn full bleed) ------------------
let icon = render(180, 180) { _ in mark(NSRect(x: 0, y: 0, width: 180, height: 180), corner: 0) }
save(icon, iconPath, .png, 1)
print("wrote \(cardPath) (\(W)x\(H)) and \(iconPath) (180x180)")
