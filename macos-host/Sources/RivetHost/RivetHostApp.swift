import SwiftUI
import AppKit

@MainActor
final class BooklightAppDelegate: NSObject, NSApplicationDelegate {
    weak var appModel: AppModel?
    private var terminationInFlight = false

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let appModel else { return .terminateNow }
        if terminationInFlight { return .terminateLater }
        terminationInFlight = true
        Task { @MainActor in
            while let reader = appModel.firstPendingPDFEditReader() {
                let alert = NSAlert()
                alert.messageText = L10n.t("ui.pdf.pending-title")
                alert.informativeText = L10n.t("ui.pdf.pending-body")
                alert.addButton(withTitle: L10n.t("ui.pdf.save-as"))
                alert.addButton(withTitle: L10n.t("ui.pdf.discard"))
                alert.addButton(withTitle: L10n.t("ui.password.cancel"))
                switch alert.runModal() {
                case .alertFirstButtonReturn:
                    guard await reader.exportEditedPdfForClose(in: nil) else {
                        terminationInFlight = false
                        sender.reply(toApplicationShouldTerminate: false)
                        return
                    }
                case .alertSecondButtonReturn:
                    reader.pdfEditsPending = false
                default:
                    terminationInFlight = false
                    sender.reply(toApplicationShouldTerminate: false)
                    return
                }
            }
            await appModel.flushAnnotationsBeforeTermination()
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }
}

@main
@MainActor
struct PDFGistApp: App {
    @NSApplicationDelegateAdaptor(BooklightAppDelegate.self) private var delegate
    @StateObject private var app: AppModel

    init() {
        let model = AppModel()
        _app = StateObject(wrappedValue: model)
        delegate.appModel = model
    }

    var body: some Scene {
        // One document per window, Preview-style: native window tabs
        // (Window > Merge All Windows), a nil value shows the welcome state
        // and a path opens straight into the document.
        WindowGroup(id: "reader", for: String.self) { $path in
            DocumentWindowView(path: path)
                .environmentObject(app)
                .frame(minWidth: 720, minHeight: 480)
        }
        .defaultSize(width: 1280, height: 820)
        .windowToolbarStyle(.unified)
        .commands { AppCommands(app: app) }

        Settings {
            SettingsSceneView()
                .environmentObject(app)
        }
    }
}

/// Menu bar following the Preview layout: File (new/open/open recent/print),
/// Edit (find), View (display modes, zoom, split, bookmark, TTS), Go (pages).
struct AppCommands: Commands {
    let app: AppModel

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button(L10n.t("ui.menu.new-window")) {
                app.openWindow?(nil)
            }
            .keyboardShortcut("n")

            Button(L10n.t("ui.menu.open")) {
                HostActions.open?()
            }
            .keyboardShortcut("o")

            openRecentMenu

            Divider()

            Button(L10n.t("ui.dialog.save") + " PDF…") {
                HostActions.exportPDF?()
            }
            .keyboardShortcut("s", modifiers: [.command, .shift])
            .disabled(app.activeReader?.pdf == nil)

            Divider()

            Button(L10n.t("ui.menu.print")) {
                HostActions.printDocument?()
            }
            .keyboardShortcut("p")
        }

        CommandGroup(after: .appInfo) {
            Button(L10n.t("ui.update.check-menu")) {
                app.checkForUpdates()
            }
            .keyboardShortcut("u")
        }

        CommandGroup(after: .textEditing) {
            Button(L10n.t("ui.menu.find")) {
                HostActions.find?()
            }
            .keyboardShortcut("f")
        }

        CommandMenu(L10n.t("ui.menu.view")) {
            Button(L10n.t("ui.menu.single-page")) {
                HostActions.setDisplayMode?(false)
            }
            .keyboardShortcut("1")
            Button(L10n.t("ui.menu.double-page")) {
                HostActions.setDisplayMode?(true)
            }
            .keyboardShortcut("2")
            Divider()
            Button(L10n.t("ui.menu.zoom-in")) {
                HostActions.zoomIn?()
            }
            .keyboardShortcut("=")
            Button(L10n.t("ui.menu.zoom-out")) {
                HostActions.zoomOut?()
            }
            .keyboardShortcut("-")
            Button(L10n.t("ui.menu.actual-size")) {
                HostActions.zoomReset?()
            }
            .keyboardShortcut("0")
            Button(L10n.t("ui.viewer.fit")) {
                HostActions.fitWidth?()
            }
            Divider()
            Toggle(L10n.t("ui.toolbar.split"), isOn: Binding(
                get: { app.activeReader?.pdf?.splitOn ?? false },
                set: { _ in HostActions.toggleSplit?() }))
            Divider()
            Button(L10n.t("ui.menu.bookmark")) {
                HostActions.toggleBookmark?()
            }
            .keyboardShortcut("b")
            Button(L10n.t("ui.menu.tts")) {
                HostActions.toggleTTS?()
            }
        }

        CommandMenu(L10n.t("ui.menu.go")) {
            Button(L10n.t("ui.menu.prev-page")) {
                HostActions.page?(-1)
            }
            .keyboardShortcut(.leftArrow, modifiers: .command)
            Button(L10n.t("ui.menu.next-page")) {
                HostActions.page?(1)
            }
            .keyboardShortcut(.rightArrow, modifiers: .command)
            Divider()
            Button(L10n.t("ui.menu.first-page")) {
                HostActions.firstPage?()
            }
            Button(L10n.t("ui.menu.last-page")) {
                HostActions.lastPage?()
            }
        }

        // Document tools: everything that CHANGES the document. Page surgery
        // is the product's core tool set — discoverable here with full
        // labels, never bolted onto reading chrome as permanent buttons.
        CommandMenu(L10n.t("ui.menu.tools")) {
            Button(L10n.t("ui.pages.rotate")) {
                HostActions.pageEdit?(.rotateCurrent)
            }
            Button(L10n.t("ui.pages.blank")) {
                HostActions.pageEdit?(.insertBlankAfterCurrent)
            }
            Button(L10n.t("ui.pages.delete")) {
                HostActions.pageEdit?(.deleteCurrent)
            }
            Divider()
            Button(L10n.t("ui.pages.extract")) {
                HostActions.pageEdit?(.extractCurrent)
            }
            Button(L10n.t("ui.pages.merge")) {
                HostActions.mergePDF?()
            }
            Divider()
            Button(L10n.t("ui.tools.watermark")) {
                HostActions.showWatermark?()
            }
            Button(L10n.t("ui.tools.export-page")) {
                HostActions.exportPageImage?()
            }
            Button(L10n.t("ui.tools.export-text")) {
                HostActions.exportText?()
            }
            Divider()
            Button(L10n.t("ui.menu.fill-forms")) {
                HostActions.showForms?()
            }
        }
    }

    /// Standard File > Open Recent, driven by the backend's recents list (it
    /// keeps the resume page); mirroring into NSDocumentController keeps the
    /// Dock's recent-files list in sync.
    @ViewBuilder
    private var openRecentMenu: some View {
        Menu(L10n.t("ui.menu.open-recent")) {
            ForEach(Array(app.recents.prefix(10).enumerated()), id: \.element.path) { _, entry in
                Button((entry.path as NSString).lastPathComponent) {
                    app.openWindow?(entry.path)
                }
            }
        }
        .disabled(app.recents.isEmpty)
    }
}
