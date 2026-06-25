import Foundation

/// Glyph sets, mirroring the original GLMatrix "Matrix encoding" popup, plus a
/// modern full-width Unicode katakana mode.
enum GlyphEncoding: Int, CaseIterable, Codable {
    case matrix, binary, hexadecimal, decimal, dna, unicode

    var title: String {
        switch self {
        case .matrix:      return "Matrix"
        case .binary:      return "Binary"
        case .hexadecimal: return "Hexadecimal"
        case .decimal:     return "Decimal"
        case .dna:         return "DNA"
        case .unicode:     return "Unicode katakana"
        }
    }
}

/// All user-tunable parameters. Sliders are stored normalized in 0...1 so the UI
/// (and the original GLMatrix Low↔High sliders) map directly; the simulation
/// converts to concrete counts/speeds via the computed helpers below.
struct Settings: Equatable, Codable {
    var density: Double = 0.42     // → strip count
    var speed: Double = 0.08       // → fall speed (deliberately a slow drip by default)

    var encoding: GlyphEncoding = .matrix

    var fog = true
    var waves = true
    var panning = false
    var textured = true
    var wireframe = false
    var showFPS = false

    // Modern additions.
    var bloom = true
    var bloomIntensity: Double = 0.53
    var hdr = true

    // MARK: Derived values

    /// The strip-count / fall-speed / mutation-rate mappings, the simulation, the glyph
    /// encodings and the world constants now live in the shared C engine (`mmcore`), so
    /// macOS and the Windows port derive the rain's behaviour identically. This converts
    /// the Swift settings (which back the SwiftUI form + JSON persistence) into the C
    /// `MMSettings` struct the engine consumes.
    var mm: MMSettings {
        var m = MMSettings()
        m.density = density
        m.speed = speed
        m.bloomIntensity = bloomIntensity
        m.encoding = Int32(encoding.rawValue)
        m.fog = fog ? 1 : 0
        m.waves = waves ? 1 : 0
        m.panning = panning ? 1 : 0
        m.textured = textured ? 1 : 0
        m.wireframe = wireframe ? 1 : 0
        m.showFPS = showFPS ? 1 : 0
        m.bloom = bloom ? 1 : 0
        m.hdr = hdr ? 1 : 0
        return m
    }

    /// Always render at the display's native refresh rate (0). The user-facing frame-rate
    /// cap was removed; this remains as the value the display-link drivers request.
    var fpsCap: Int { 0 }
}

/// Settings persisted as a JSON file, SHARED between the app and the screensaver.
/// The saver runs sandboxed inside legacyScreenSaver, so its `defaults` /
/// `ScreenSaverDefaults` are silently redirected to a private container the external
/// app cannot reach. Writing a plain file at the saver's own container path (see
/// SaverSettingsFile) is the reliable way for the app to actually configure the saver.
final class SettingsStore {
    private let url: URL
    init(url: URL) { self.url = url }

    var settings: Settings {
        get { SaverSettingsFile.read(at: url) ?? Settings() }
        set { SaverSettingsFile.write(newValue, to: url) }
    }
}

extension SettingsStore {
    /// Store for the companion app — writes the saver's container file via an absolute path.
    static func appStore() -> SettingsStore { SettingsStore(url: SaverSettingsFile.appURL) }
    /// Store for the screensaver itself — its sandbox home maps to that same file.
    static func saverStore() -> SettingsStore { SettingsStore(url: SaverSettingsFile.saverURL) }
}
