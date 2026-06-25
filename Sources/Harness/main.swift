import AppKit
import Metal
import QuartzCore
import SwiftUI

// RainView (the live Metal preview) now lives in Sources/Core/RainView.swift so the
// saver's Options sheet can use it too.

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    let store = SettingsStore.appStore()
    private lazy var model = SettingsModel(store: store)

    func applicationDidFinishLaunching(_ note: Notification) {
        let rect = NSRect(x: 0, y: 0, width: 1160, height: 740)
        window = NSWindow(contentRect: rect,
                          styleMask: [.titled, .closable, .resizable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "Modern Matrix — Preview & Settings"
        window.center()

        let split = NSSplitView(frame: rect)
        split.isVertical = true
        split.dividerStyle = .thin
        split.autoresizingMask = [.width, .height]

        let settings = NSHostingView(rootView: ConfigureView(model: model))
        settings.setFrameSize(NSSize(width: 400, height: rect.height))
        let rain = RainView(frame: rect, model: model)

        split.addSubview(settings)
        split.addSubview(rain)
        window.contentView = split
        window.makeKeyAndOrderFront(nil)
        split.setPosition(400, ofDividerAt: 0)

        buildMenu()
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

    private func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Toggle Full Screen",
                        action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
            .keyEquivalentModifierMask = [.command, .control]
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit Modern Matrix",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = main
    }
}

// MARK: - Entry

let args = CommandLine.arguments
if let i = args.firstIndex(of: "--snapshot") {
    let outPath = (i + 1 < args.count && !args[i + 1].hasPrefix("--")) ? args[i + 1] : "snapshot.png"
    func flag(_ f: String) -> Bool { args.contains(f) }
    func val(_ f: String) -> String? {
        if let k = args.firstIndex(of: f), k + 1 < args.count { return args[k + 1] }; return nil
    }
    var s = SettingsStore.appStore().settings
    if let e = val("--encoding"), let n = Int(e), let enc = GlyphEncoding(rawValue: n) { s.encoding = enc }
    if let d = val("--density"), let v = Double(d) { s.density = v }
    if let sp = val("--speed"), let v = Double(sp) { s.speed = v }
    if flag("--wireframe") { s.wireframe = true }
    if flag("--no-bloom") { s.bloom = false }
    if flag("--no-fog") { s.fog = false }
    if flag("--no-textured") { s.textured = false }
    if flag("--no-waves") { s.waves = false }
    if flag("--panning") { s.panning = true }
    let warmup = Int(val("--warmup") ?? "") ?? 90
    guard let device = MTLCreateSystemDefaultDevice() else { fatalError("No Metal device") }
    guard let renderer = Renderer(device: device, bundle: .main, settings: s),
          let image = renderer.renderImage(width: 1600, height: 1000, warmupFrames: warmup) else {
        fatalError("Render failed")
    }
    let rep = NSBitmapImageRep(cgImage: image)
    guard let png = rep.representation(using: .png, properties: [:]) else { fatalError("PNG encode failed") }
    do {
        try png.write(to: URL(fileURLWithPath: outPath))
        FileHandle.standardError.write(Data("Wrote snapshot to \(outPath)\n".utf8))
    } catch { fatalError("Write failed: \(error)") }
    exit(0)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
