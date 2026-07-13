import AppKit
import Metal
import QuartzCore

/// A layer-backed Metal view that renders the rain into a CAMetalLayer, reading the
/// shared `SettingsModel` each frame so slider changes preview live.
///
/// Used by BOTH the companion app and the saver's Options sheet (which runs inside
/// System Settings' process), so it has to be robust anywhere: a view-bound
/// CADisplayLink drives it, with a timer fallback if the link doesn't fire, both
/// funnelling through one render path that derives dt from a single clock (so they
/// can't double-advance). Shaders load from `Bundle(for: RainView.self)`, which
/// resolves to whichever bundle this code is compiled into (app or .saver).
final class RainView: NSView {
    private var renderer: Renderer?
    private var driver: DisplayLinkDriver?
    private var fallbackTimer: Timer?
    private var overlay: FPSOverlay?
    private let model: SettingsModel
    private var lastApplied: Settings?
    private var lastRenderTime: CFTimeInterval = 0
    private var lastLinkRender: CFTimeInterval = 0

    init(frame: CGRect, model: SettingsModel) {
        self.model = model
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .duringViewResize
    }
    required init?(coder: NSCoder) { fatalError() }

    override func makeBackingLayer() -> CALayer {
        let l = CAMetalLayer()
        l.device = MTLCreateSystemDefaultDevice()
        l.pixelFormat = .bgra8Unorm
        l.framebufferOnly = true
        l.isOpaque = true
        l.backgroundColor = NSColor.black.cgColor
        return l
    }
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { stop() } else { start() }
    }

    private func start() {
        ensureRenderer()
        if overlay == nil { overlay = FPSOverlay(parent: metalLayer) }
        updateDrawableSize()
        if driver == nil {
            let d = DisplayLinkDriver(view: self) { [weak self] _ in self?.linkTick() }
            d.start(preferredFPS: model.s.fpsCap)
            driver = d
        }
        if fallbackTimer == nil {
            let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in self?.timerTick() }
            RunLoop.main.add(t, forMode: .common)
            fallbackTimer = t
        }
    }

    private func stop() {
        driver?.stop(); driver = nil
        fallbackTimer?.invalidate(); fallbackTimer = nil
    }

    private func ensureRenderer() {
        guard renderer == nil, let device = metalLayer.device else { return }
        let r = Renderer(device: device, bundle: Bundle(for: RainView.self), settings: model.s)
        r?.configure(layer: metalLayer)
        renderer = r
        lastApplied = model.s
    }

    private func linkTick() { lastLinkRender = CACurrentMediaTime(); renderFrame() }
    private func timerTick() { if CACurrentMediaTime() - lastLinkRender > 0.25 { renderFrame() } }

    private func renderFrame() {
        guard let r = renderer else { return }
        let now = CACurrentMediaTime()
        let elapsed = lastRenderTime == 0 ? (1.0 / 120.0) : (now - lastRenderTime)
        if lastRenderTime != 0 && elapsed < 0.0015 { return }   // already drew this frame
        lastRenderTime = now
        let dt = Float(min(0.1, max(0, elapsed)))
        let s = model.s
        if s != lastApplied {
            r.apply(s)
            r.configure(layer: metalLayer)
            lastApplied = s
            driver?.start(preferredFPS: s.fpsCap)
        }
        r.draw(in: metalLayer, dt: dt)
        overlay?.update(fps: r.fps, visible: s.showFPS, scale: window?.backingScaleFactor ?? 2)
    }

    override func viewDidChangeBackingProperties() { super.viewDidChangeBackingProperties(); updateDrawableSize() }
    override func layout() { super.layout(); updateDrawableSize() }

    private func updateDrawableSize() {
        let scale = window?.backingScaleFactor ?? 2
        metalLayer.contentsScale = scale
        let px = CGSize(width: bounds.width * scale, height: bounds.height * scale)
        if px.width > 0, px.height > 0 {
            metalLayer.drawableSize = px
            renderer?.viewportSize = px
        }
    }
}
