import AppKit
import QuartzCore

/// Wraps a view-bound CADisplayLink (macOS 14+) and hands back a clamped delta
/// time each tick. Used by both the harness view and the screensaver view.
final class DisplayLinkDriver: NSObject {
    private var link: CADisplayLink?
    private weak var view: NSView?
    private let onTick: (Double) -> Void
    private var last: CFTimeInterval = 0

    init(view: NSView, onTick: @escaping (Double) -> Void) {
        self.view = view
        self.onTick = onTick
        super.init()
    }

    func start(preferredFPS: Int) {
        stop()
        guard let view else { return }
        let l = view.displayLink(target: self, selector: #selector(tick(_:)))
        if preferredFPS > 0 {
            let hi = Float(preferredFPS)
            l.preferredFrameRateRange = CAFrameRateRange(
                minimum: min(hi, 30), maximum: hi, preferred: hi)
        }
        l.add(to: .main, forMode: .common)
        last = 0
        link = l
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick(_ l: CADisplayLink) {
        let now = l.timestamp
        let dt = last == 0 ? 1.0 / 60.0 : max(0, min(0.1, now - last))
        last = now
        onTick(dt)
    }
}
