import AppKit

// Renders the WIREVOX app icon (RACK '84 faceplate) to a 1024x1024 PNG.
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png"
let size: CGFloat = 1024
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255, blue: CGFloat(hex & 0xff) / 255, alpha: a)
}

// macOS icon grid: 824 pt body centred on a 1024 canvas.
let body = NSRect(x: 100, y: 100, width: 824, height: 824)
let plate = NSBezierPath(roundedRect: body, xRadius: 186, yRadius: 186)

ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: NSColor.black.withAlphaComponent(0.45).cgColor)
rgb(0x2b2d31).setFill(); plate.fill()
ctx.restoreGState()

ctx.saveGState()
plate.addClip()
NSGradient(colors: [rgb(0x4a4d53), rgb(0x2f3136), rgb(0x1f2024)])!.draw(in: body, angle: -90)

// Brushed metal: thin horizontal streaks of varying brightness.
var seed: UInt64 = 84
func rnd() -> CGFloat { seed = seed &* 6364136223846793005 &+ 1442695040888963407; return CGFloat((seed >> 33) % 10000) / 10000 }
var y = body.minY
while y < body.maxY {
    let h = 1 + rnd() * 2
    (rnd() > 0.5 ? NSColor.white : NSColor.black).withAlphaComponent(0.02 + rnd() * 0.05).setFill()
    NSRect(x: body.minX, y: y, width: body.width, height: h).fill()
    y += h
}

// Accent stripes under the top edge.
rgb(0xe8823a).setFill(); NSRect(x: body.minX, y: 742, width: body.width, height: 22).fill()
rgb(0xe9dfc4).setFill(); NSRect(x: body.minX, y: 722, width: body.width, height: 8).fill()
ctx.restoreGState()

// Bevel.
let inner = NSBezierPath(roundedRect: body.insetBy(dx: 5, dy: 5), xRadius: 181, yRadius: 181)
inner.lineWidth = 6; NSColor.white.withAlphaComponent(0.13).setStroke(); inner.stroke()

// LCD window with a voice-level bar display.
let lcd = NSRect(x: 196, y: 392, width: 632, height: 280)
let bezel = NSBezierPath(roundedRect: lcd.insetBy(dx: -14, dy: -14), xRadius: 34, yRadius: 34)
rgb(0x121316).setFill(); bezel.fill()
let glass = NSBezierPath(roundedRect: lcd, xRadius: 22, yRadius: 22)
ctx.saveGState(); glass.addClip()
NSGradient(colors: [rgb(0x0d1f12), rgb(0x06120a)])!.draw(in: lcd, angle: -90)
let levels: [CGFloat] = [0.28, 0.5, 0.82, 0.62, 1.0, 0.7, 0.9, 0.46, 0.66, 0.34, 0.2]
let barW: CGFloat = 34, gap: CGFloat = 20
let total = CGFloat(levels.count) * barW + CGFloat(levels.count - 1) * gap
var x = lcd.midX - total / 2
for level in levels {
    let h = (lcd.height - 72) * level
    let bar = NSRect(x: x, y: lcd.midY - h / 2, width: barW, height: h)
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 22, color: rgb(0xffb347, 0.9).cgColor)
    rgb(0xffb347).setFill(); NSBezierPath(roundedRect: bar, xRadius: 8, yRadius: 8).fill()
    ctx.restoreGState()
    x += barW + gap
}
var sy = lcd.minY
while sy < lcd.maxY { NSColor.black.withAlphaComponent(0.22).setFill(); NSRect(x: lcd.minX, y: sy, width: lcd.width, height: 3).fill(); sy += 8 }
ctx.restoreGState()

// Silkscreen logo.
let style = NSMutableParagraphStyle(); style.alignment = .center
let font = NSFont(name: "HelveticaNeue-CondensedBlack", size: 150) ?? NSFont.systemFont(ofSize: 150, weight: .black)
let text = NSAttributedString(string: "VOX", attributes: [.font: font, .foregroundColor: rgb(0xe9dfc4), .kern: 34, .paragraphStyle: style])
text.draw(in: NSRect(x: body.minX + 17, y: 196, width: body.width, height: 170))

// Rack screws.
for (cx, cy) in [(196.0, 822.0), (828.0, 822.0), (196.0, 176.0), (828.0, 176.0)] {
    let r = NSRect(x: cx - 24, y: cy - 24, width: 48, height: 48)
    NSGradient(colors: [rgb(0xd0d2d6), rgb(0x6f7278)])!.draw(in: NSBezierPath(ovalIn: r), angle: -60)
    let slot = NSBezierPath(); slot.move(to: NSPoint(x: cx - 15, y: cy - 9)); slot.line(to: NSPoint(x: cx + 15, y: cy + 9))
    slot.lineWidth = 7; rgb(0x2a2b2e).setStroke(); slot.stroke()
}

NSGraphicsContext.current!.flushGraphics()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
