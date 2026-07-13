import Foundation

/// The single JSON file the app and the (sandboxed) screensaver share.
///
/// The screensaver runs inside `legacyScreenSaver`, whose sandbox home maps to
/// `~/Library/Containers/com.apple.ScreenSaver.Engine.legacyScreenSaver/Data`.
/// The saver reaches the file via `NSHomeDirectory()`; the unsandboxed app reaches
/// the exact same file via the absolute container path. This sidesteps the
/// `defaults`/cfprefsd container redirection that prevented the app's settings from
/// ever reaching the saver.
enum SaverSettingsFile {
    private static let fileName = "matrixrain-settings.json"
    private static let containerHome =
        "Library/Containers/com.apple.ScreenSaver.Engine.legacyScreenSaver/Data"

    /// Absolute path used by the UNSANDBOXED app to reach the saver's container file.
    static var appURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(containerHome)
            .appendingPathComponent(fileName)
    }

    /// Path used by the SANDBOXED saver — its container home is that same directory.
    static var saverURL: URL {
        URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(fileName)
    }

    static func read(at url: URL) -> Settings? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(Settings.self, from: data)
    }

    static func write(_ settings: Settings, to url: URL) {
        guard let data = try? JSONEncoder().encode(settings) else { return }
        try? FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: url, options: .atomic)
    }
}
