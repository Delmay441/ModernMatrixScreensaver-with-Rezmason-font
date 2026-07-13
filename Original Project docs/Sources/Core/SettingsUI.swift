import SwiftUI
import AppKit

/// Holds a *working* copy of the settings that drives the live preview instantly,
/// and only writes to the backing store (the screensaver's settings domain) when
/// `save()` is called — so the user explicitly commits changes to the screensaver.
final class SettingsModel: ObservableObject {
    @Published var s: Settings                      // working copy → live preview
    @Published private(set) var savedSnapshot: Settings
    @Published var lastSavedAt: Date?
    private let store: SettingsStore

    init(store: SettingsStore) {
        self.store = store
        let loaded = store.settings
        self.s = loaded
        self.savedSnapshot = loaded
    }

    var isDirty: Bool { s != savedSnapshot }

    func save() {
        store.settings = s
        savedSnapshot = s
        lastSavedAt = Date()
    }

    /// Reset the working copy to defaults (preview updates; not committed until Save).
    func reset() { s = Settings() }
}

struct ConfigureView: View {
    @ObservedObject var model: SettingsModel
    var onDone: (() -> Void)? = nil

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss"; return f
    }()

    private static let blogURL = URL(string: "https://www.chewie.co.uk/coding/modern-matrix-screensaver/")!
    private static let githubURL = URL(string: "https://github.com/DigitalChewie/ModernMatrixScreensaver")!

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 8) {
                Image(systemName: "chevron.left.forwardslash.chevron.right")
                    .foregroundStyle(.green)
                Text("Modern Matrix").font(.headline)
                Spacer()
            }

            slider("Glyph density", "Sparse", "Dense", $model.s.density)
            slider("Glyph speed", "Slow", "Fast", $model.s.speed)

            Picker("Matrix encoding", selection: $model.s.encoding) {
                ForEach(GlyphEncoding.allCases, id: \.self) { Text($0.title).tag($0) }
            }
            .pickerStyle(.menu)

            Divider()

            LazyVGrid(columns: [GridItem(.flexible(), alignment: .leading),
                                GridItem(.flexible(), alignment: .leading)], spacing: 6) {
                Toggle("Fog", isOn: $model.s.fog)
                Toggle("Waves", isOn: $model.s.waves)
                Toggle("Panning", isOn: $model.s.panning)
                Toggle("Textured", isOn: $model.s.textured)
                Toggle("Wireframe", isOn: $model.s.wireframe)
                Toggle("Show frame rate", isOn: $model.s.showFPS)
                Toggle("Bloom glow", isOn: $model.s.bloom)
                Toggle("HDR highlights", isOn: $model.s.hdr)
            }
            .toggleStyle(.checkbox)

            if model.s.bloom {
                slider("Glow intensity", "Subtle", "Intense", $model.s.bloomIntensity)
            }

            Divider()

            status

            HStack {
                Button("Reset to Defaults") { model.reset() }
                Spacer()
                if let onDone {
                    Button("Done") { onDone() }
                }
                Button("Save") { model.save() }
                    .buttonStyle(.borderedProminent)
                    .disabled(!model.isDirty)
                    .keyboardShortcut("s", modifiers: .command)
            }

            HStack(spacing: 10) {
                Link("Blog post", destination: Self.blogURL)
                Text("·").foregroundStyle(.tertiary)
                Link("Source on GitHub", destination: Self.githubURL)
                Spacer()
            }
            .font(.caption)
        }
        .padding(20)
        .frame(width: 380)
    }

    @ViewBuilder private var status: some View {
        HStack(spacing: 6) {
            if model.isDirty {
                Image(systemName: "pencil.circle.fill").foregroundStyle(.orange)
                Text("Unsaved changes — click Save to apply to the screensaver")
                    .foregroundStyle(.secondary)
            } else if let when = model.lastSavedAt {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                Text("Saved at \(Self.timeFormatter.string(from: when)) — applied to the screensaver")
                    .foregroundStyle(.secondary)
            } else {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                Text("Matches the screensaver's saved settings").foregroundStyle(.secondary)
            }
            Spacer()
        }
        .font(.caption)
    }

    private func slider(_ title: String, _ lo: String, _ hi: String,
                        _ value: Binding<Double>) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.subheadline)
            HStack(spacing: 8) {
                Text(lo).font(.caption).foregroundStyle(.secondary)
                Slider(value: value, in: 0...1)
                Text(hi).font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}
