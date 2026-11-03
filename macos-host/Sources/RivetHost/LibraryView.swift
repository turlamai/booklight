import SwiftUI
import AppKit

/// The empty-window state. Booklight opens documents, not a managed library.
/// Recent paths are shortcuts only; no covers, collections, reading stats or
/// document copies are produced by this view.
struct FileWelcomeView: View {
    @EnvironmentObject private var app: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            Spacer()
            Text("Booklight")
                .font(.system(size: 38, weight: .semibold, design: .rounded))
                .foregroundStyle(Theme.text)
                .accessibilityAddTraits(.isHeader)

            Text(L10n.t("ui.welcome.open-any"))
                .font(.title3)
                .foregroundStyle(.secondary)

            Button {
                HostActions.open?()
            } label: {
                Label(L10n.t("ui.menu.open"), systemImage: "folder")
                    .font(.headline)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 7)
            }
            .buttonStyle(.borderedProminent)
            .accessibilityIdentifier("booklight.welcome.open")

            Text(L10n.t("ui.welcome.or-drag"))
                .font(.subheadline)
                .foregroundStyle(.secondary)

            if !app.recents.isEmpty {
                Divider()
                Text(L10n.t("ui.viewer.recents"))
                    .font(.headline)
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 4) {
                        ForEach(Array(app.recents.prefix(10)), id: \.path) { entry in
                            Button {
                                app.openWindow?(entry.path)
                            } label: {
                                HStack(spacing: 10) {
                                    Image(systemName: "doc.text")
                                        .foregroundStyle(.secondary)
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(URL(fileURLWithPath: entry.path).lastPathComponent)
                                            .lineLimit(1)
                                        Text(entry.path)
                                            .font(.caption)
                                            .foregroundStyle(.secondary)
                                            .lineLimit(1)
                                    }
                                    Spacer()
                                }
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .padding(.vertical, 5)
                        }
                    }
                }
                .frame(maxHeight: 240)
            }
            Spacer()
        }
        .frame(maxWidth: 600, alignment: .leading)
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg)
    }
}
