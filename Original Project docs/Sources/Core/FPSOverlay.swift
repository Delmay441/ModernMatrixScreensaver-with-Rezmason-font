import QuartzCore
import AppKit

/// A tiny CATextLayer that floats the current frame rate over the Metal layer,
/// mirroring GLMatrix's "Show frame rate" option.
final class FPSOverlay {
    private let text = CATextLayer()

    init(parent: CALayer) {
        text.fontSize = 14
        text.font = NSFont.monospacedDigitSystemFont(ofSize: 14, weight: .medium)
        text.foregroundColor = NSColor(red: 0.4, green: 1, blue: 0.5, alpha: 1).cgColor
        text.alignmentMode = .left
        text.anchorPoint = .zero
        text.position = CGPoint(x: 14, y: 12)
        text.bounds = CGRect(x: 0, y: 0, width: 120, height: 20)
        text.isHidden = true
        parent.addSublayer(text)
    }

    func update(fps: Double, visible: Bool, scale: CGFloat) {
        text.isHidden = !visible
        guard visible else { return }
        text.contentsScale = scale
        text.string = String(format: "%.0f FPS", fps)
    }
}
