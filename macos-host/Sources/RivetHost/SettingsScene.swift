import SwiftUI

/// The Settings scene (⌘, / app menu). Provider configuration, reading
/// preferences and privacy — everything that is app-wide, not per document.
struct SettingsSceneView: View {
    var body: some View {
        SettingsPane()
            .frame(width: 560, height: 680)
    }
}
