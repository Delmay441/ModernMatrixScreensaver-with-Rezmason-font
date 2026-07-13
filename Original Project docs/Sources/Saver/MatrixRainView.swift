import ScreenSaver
import Metal
import QuartzCore
import AppKit

/// The screensaver entry point. NSPrincipalClass in Info.plist points here.
///
/// Hardened against the known Sonoma/Sequoia/Tahoe host bugs: rendering is driven
/// by a view-bound CADisplayLink (which stops automatically when the view leaves
/// its window), every instance owns its own state (no global leaks across the
/// host's repeated instantiations), and we tear down in both stopAnimation and
/// viewDidMoveToWindow(nil) since stopAnimation is no longer reliably called.
@objc(MatrixRainView)
final class MatrixRainView: ScreenSaverView {
    private var renderer: Renderer?
    private var driver: DisplayLinkDriver?
    private var overlay: FPSOverlay?
    private let store: SettingsStore
    private var lastApplied: Settings?
    private var configWindow: NSWindow?
    private var lastRenderTime: CFTimeInterval = 0
    private var lastLinkRender: CFTimeInterval = 0

    override init?(frame: NSRect, isPreview: Bool) {
        self.store = SettingsStore.saverStore()
        super.init(frame: frame, isPreview: isPreview)
        wantsLayer = true
        animationTimeInterval = 1.0 / 30.0   // replaced in startAnimation
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) unused") }

    override func makeBackingLayer() -> CALayer {
        let l = CAMetalLayer()
        l.device = MTLCreateSystemDefaultDevice()
        l.pixelFormat = .bgra8Unorm
        l.framebufferOnly = true
        l.isOpaque = true
        l.backgroundColor = NSColor.black.cgColor
        return l
    }

    private var metalLayer: CAMetalLayer? { layer as? CAMetalLayer }

    private var cachedSettings = Settings()
    private var lastSettingsLoad: CFTimeInterval = -1

    private func currentSettings() -> Settings {
        // Re-read the shared file at most ~once a second so app edits get picked up.
        let now = CACurrentMediaTime()
        if lastSettingsLoad < 0 || now - lastSettingsLoad > 0.75 {
            lastSettingsLoad = now
            cachedSettings = store.settings
        }
        var s = cachedSettings
        if isPreview {                 // the tiny System Settings thumbnail
            s.density = min(s.density, 0.35)
            s.bloom = false
        }
        return s
    }

    private func ensureRenderer() {
        guard renderer == nil, let device = metalLayer?.device else { return }
        let s = currentSettings()
        renderer = Renderer(device: device, bundle: Bundle(for: MatrixRainView.self), settings: s)
        if let metalLayer { renderer?.configure(layer: metalLayer) }
        lastApplied = s
    }

    private func updateDrawableSize() {
        guard let metalLayer else { return }
        let scale = window?.backingScaleFactor ?? 2
        metalLayer.contentsScale = scale
        let px = CGSize(width: bounds.width * scale, height: bounds.height * scale)
        if px.width > 0, px.height > 0 {
            metalLayer.drawableSize = px
            renderer?.viewportSize = px
        }
    }

    // MARK: Animation lifecycle

    override func startAnimation() {
        super.startAnimation()
        let cap = currentSettings().fpsCap
        animationTimeInterval = 1.0 / Double(cap > 0 ? cap : 120)   // timer-fallback cadence
        ensureRenderer()
        if overlay == nil, let metalLayer { overlay = FPSOverlay(parent: metalLayer) }
        updateDrawableSize()
        startLink()
    }

    override func stopAnimation() {
        super.stopAnimation()
        stopLink()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { stopLink() } else { updateDrawableSize(); startLink() }
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    override func layout() {
        super.layout()
        updateDrawableSize()
    }

    // The display link is an optimisation we attempt when we have a window; the
    // host timer (animateOneFrame) is the baseline that always renders whenever
    // the link isn't actively ticking — legacyScreenSaver frequently never fires it.
    private func startLink() {
        guard driver == nil, window != nil else { return }
        ensureRenderer()
        let d = DisplayLinkDriver(view: self) { [weak self] dt in self?.linkTick(dt) }
        d.start(preferredFPS: currentSettings().fpsCap)
        driver = d
    }

    private func stopLink() {
        driver?.stop()
        driver = nil
    }

    private func linkTick(_ dt: Double) {
        lastLinkRender = CACurrentMediaTime()
        renderFrame()
    }

    override func animateOneFrame() {
        // The host timer only drives when the display link isn't (it often never
        // fires inside legacyScreenSaver). Both funnel through renderFrame().
        if CACurrentMediaTime() - lastLinkRender < 0.25 { return }
        renderFrame()
    }

    /// Single render entry point. dt comes from ONE shared clock, so even if both
    /// the display link and the host timer fire, the simulation can never advance
    /// by more than the real elapsed time — no frame-rate-dependent speedup.
    private func renderFrame() {
        guard let renderer, let metalLayer else { return }
        let now = CACurrentMediaTime()
        let elapsed = lastRenderTime == 0 ? (1.0 / 120.0) : (now - lastRenderTime)
        if lastRenderTime != 0 && elapsed < 0.0015 { return }   // already drew this frame
        lastRenderTime = now
        let dt = Float(min(0.1, max(0, elapsed)))
        let s = currentSettings()
        if s != lastApplied {
            renderer.apply(s)
            renderer.configure(layer: metalLayer)
            lastApplied = s
            driver?.start(preferredFPS: s.fpsCap)
        }
        renderer.draw(in: metalLayer, dt: dt)
        overlay?.update(fps: renderer.fps, visible: s.showFPS, scale: window?.backingScaleFactor ?? 2)
    }

    // MARK: Configuration sheet

    override var hasConfigureSheet: Bool { true }

    override var configureSheet: NSWindow? {
        let w = ConfigureSheet.makeWindow(store: store)
        configWindow = w
        return w
    }

    deinit { stopLink() }
}
