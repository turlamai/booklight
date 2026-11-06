import SwiftUI
import AppKit

/// The empty-window state. Booklight opens documents, not a managed library.
/// Recent paths are shortcuts only; no covers, collections, reading stats or
/// document copies are produced by this view.
///
/// Layout: an editorial left column (mark, promise, one primary action) and a
/// right column of recent files. On narrow windows the two columns stack so
/// the primary action is never pushed below the fold.
struct FileWelcomeView: View {
    @EnvironmentObject private var app: AppModel
    @State private var hoveredPath: String?

    private var recents: [RivetTypes.RecentEntry] {
        Array(app.recents.prefix(8))
    }

    var body: some View {
        GeometryReader { proxy in
            ScrollView {
                Group {
                    if proxy.size.width >= 880 {
                        HStack(alignment: .top, spacing: 64) {
                            intro.frame(width: 360, alignment: .leading)
                            recentColumn.frame(maxWidth: 500, alignment: .leading)
                        }
                    } else {
                        VStack(alignment: .leading, spacing: 36) {
                            intro
                            recentColumn
                        }
                        .frame(maxWidth: 520, alignment: .leading)
                    }
                }
                .frame(maxWidth: 980, alignment: .center)
                .padding(.horizontal, 48)
                .padding(.vertical, 56)
                .frame(minHeight: proxy.size.height, alignment: .center)
                .frame(maxWidth: .infinity)
            }
            .scrollIndicators(.hidden)
        }
        .background(Theme.bg)
        .navigationTitle(L10n.t("app.name"))
        .accessibilityIdentifier("booklight.welcome")
    }

    // MARK: left column

    private var intro: some View {
        VStack(alignment: .leading, spacing: 22) {
            BooklightMark()

            VStack(alignment: .leading, spacing: 8) {
                Text(L10n.t("app.name"))
                    .font(.system(size: 36, weight: .semibold))
                    .tracking(-0.7)
                    .accessibilityAddTraits(.isHeader)

                Text(L10n.t("ui.welcome.open-any"))
                    .font(.system(size: 17))
                    .foregroundStyle(Theme.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Button {
                HostActions.open?()
            } label: {
                Label(L10n.t("ui.welcome.open-document"), systemImage: "folder")
                    .font(.system(size: 14, weight: .medium))
                    .frame(minWidth: 128)
                    .padding(.vertical, 2)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .tint(Theme.accent)
            .accessibilityIdentifier("booklight.welcome.open")

            Label {
                Text(L10n.t("ui.welcome.or-drag"))
            } icon: {
                Image(systemName: "arrow.down.doc")
            }
            .font(.system(size: 12))
            .foregroundStyle(Theme.textDim)
            .labelStyle(.titleAndIcon)
            .padding(.bottom, 6)

            Text(L10n.t("ui.welcome.privacy"))
                .font(.footnote)
                .foregroundStyle(Theme.textFaint)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: right column

    private var recentColumn: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(L10n.t("ui.viewer.recents"))
                    .font(.headline)
                Spacer()
                if !recents.isEmpty {
                    Text("\(app.recents.count)")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(Theme.textFaint)
                }
            }

            if recents.isEmpty {
                emptyRecent
            } else {
                VStack(spacing: 0) {
                    ForEach(Array(recents.enumerated()), id: \.element.path) { index, entry in
                        recentRow(entry)
                        if index < recents.count - 1 {
                            Divider()
                                .overlay(Theme.border)
                                .padding(.leading, 48)
                        }
                    }
                }
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.large, style: .continuous)
                        .fill(Theme.panel))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.large, style: .continuous)
                        .stroke(Theme.border, lineWidth: 1))
            }
        }
    }

    private func recentRow(_ entry: RivetTypes.RecentEntry) -> some View {
        let url = URL(fileURLWithPath: entry.path)
        return Button {
            app.openWindow?(entry.path)
        } label: {
            HStack(spacing: 12) {
                Image(systemName: iconName(for: url))
                    .font(.system(size: 18, weight: .regular))
                    .foregroundStyle(Theme.accent)
                    .frame(width: 28)

                VStack(alignment: .leading, spacing: 2) {
                    Text(url.lastPathComponent)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(Theme.text)
                        .lineLimit(1)
                    Text(displayFolder(for: url))
                        .font(.caption)
                        .foregroundStyle(Theme.textFaint)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }

                Spacer(minLength: 8)

                if entry.page > 1 {
                    Text(L10n.t("ui.welcome.resume", "\(entry.page)"))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(Theme.textFaint)
                }

                Image(systemName: "chevron.right")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(Theme.textFaint)
                    .opacity(hoveredPath == entry.path ? 1 : 0)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                    .fill(hoveredPath == entry.path ? Theme.panelAlt : Color.clear)
                    .padding(.horizontal, 4))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering in
            hoveredPath = hovering ? entry.path : (hoveredPath == entry.path ? nil : hoveredPath)
        }
        .help(entry.path)
    }

    private var emptyRecent: some View {
        VStack(alignment: .leading, spacing: 8) {
            Image(systemName: "doc.text.magnifyingglass")
                .font(.system(size: 26, weight: .light))
                .foregroundStyle(Theme.textFaint)
            Text(L10n.t("ui.welcome.no-recent"))
                .font(.system(size: 13, weight: .medium))
            Text(L10n.t("ui.welcome.no-recent-hint"))
                .font(.caption)
                .foregroundStyle(Theme.textFaint)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(22)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.large, style: .continuous)
                .fill(Theme.panel))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.large, style: .continuous)
                .strokeBorder(Theme.border, style: StrokeStyle(lineWidth: 1, dash: [5, 5])))
    }

    private func iconName(for url: URL) -> String {
        url.pathExtension.lowercased() == "epub" ? "book.closed" : "doc.richtext"
    }

    private func displayFolder(for url: URL) -> String {
        let folder = url.deletingLastPathComponent().path
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        if folder == home { return "~" }
        if folder.hasPrefix(home + "/") {
            return "~" + folder.dropFirst(home.count)
        }
        return folder
    }
}

/// The product mark used on the welcome surface. It is drawn from the same
/// terracotta pair as the app icon so the first window and the Dock icon read
/// as one product, without shipping a second bitmap.
struct BooklightMark: View {
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 15, style: .continuous)
                .fill(
                    LinearGradient(
                        colors: [Theme.accent, Theme.accentHover],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing))
                .frame(width: 60, height: 60)
                .shadow(color: Theme.accent.opacity(0.22), radius: 12, y: 5)
            Image(systemName: "book.closed.fill")
                .font(.system(size: 26, weight: .semibold))
                .foregroundStyle(.white)
                .offset(y: -0.5)
        }
        .accessibilityHidden(true)
    }
}
