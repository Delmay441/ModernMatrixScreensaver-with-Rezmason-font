import SwiftUI
import AppKit

/// The Options panel macOS presents (as a sheet) when you click "Options…" in
/// System Settings: the shared settings form beside a **live Metal preview** — the
/// same split the companion app uses — so the effect of each slider/toggle is
/// visible as you change it. Both sides share one SettingsModel; the preview reads
/// the working copy live, and Save commits it to the screensaver.
enum ConfigureSheet {
    static func makeWindow(store: SettingsStore) -> NSWindow {
        let model = SettingsModel(store: store)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 600),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.title = "Modern Matrix"

        let settings = NSHostingView(rootView: ConfigureView(model: model) { [weak window] in
            model.save()
            guard let window else { return }
            if let parent = window.sheetParent { parent.endSheet(window) } else { window.close() }
        })
        let preview = RainView(frame: .zero, model: model)
        settings.translatesAutoresizingMaskIntoConstraints = false
        preview.translatesAutoresizingMaskIntoConstraints = false

        // Deterministic layout: settings fixed at 380 on the left, preview fills the rest.
        // (An NSSplitView's divider position is reset when the host re-lays-out the sheet.)
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 760, height: 600))
        container.addSubview(settings)
        container.addSubview(preview)
        NSLayoutConstraint.activate([
            settings.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            settings.topAnchor.constraint(equalTo: container.topAnchor),
            settings.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            settings.widthAnchor.constraint(equalToConstant: 380),
            preview.leadingAnchor.constraint(equalTo: settings.trailingAnchor),
            preview.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            preview.topAnchor.constraint(equalTo: container.topAnchor),
            preview.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            preview.widthAnchor.constraint(greaterThanOrEqualToConstant: 320),
        ])
        window.contentView = container
        return window
    }
}
