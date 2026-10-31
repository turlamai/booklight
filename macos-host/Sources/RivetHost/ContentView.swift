import SwiftUI
import AppKit
import PDFKit
import WebKit
import UniformTypeIdentifiers

/// One window = one document (Preview model). The window group hands us a
/// path (opened file) or nil (welcome state); each window owns a ReaderModel
/// for its lifetime.
struct DocumentWindowView: View {
    @EnvironmentObject private var app: AppModel
    @Environment(\.openWindow) private var openWindowEnv
    let path: String?

    @State private var reader: ReaderModel?

    var body: some View {
        ZStack {
            if let reader {
                ReaderWindowView(reader: reader)
            } else {
                Theme.bg.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .task {
            guard reader == nil else { return }
            app.start()
            let model = ReaderModel(app: app)
            reader = model
            app.register(model)
            if let path {
                await model.openPath(path)
            } else if let launch = app.claimPendingLaunch() {
                await model.openPath(launch)
            }
        }
        .onChange(of: path) { newPath in
            guard let newPath, let reader,
                  reader.documentPath != newPath else { return }
            Task { await reader.openPath(newPath) }
        }
    }
}

/// Bridges the SwiftUI window to AppKit: document title + proxy icon, native
/// window tabbing (Preview model), key-window tracking for the menus, and
/// close-time flush of pending persistence.
struct WindowAccessor: NSViewRepresentable {
    @ObservedObject var reader: ReaderModel
    let app: AppModel

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSView {
        context.coordinator.attach()
        return NSView()
    }

    func updateNSView(_ view: NSView, context: Context) {
        context.coordinator.parent = self
        // attach() is idempotent: the window may not be key/visible yet at
        // first layout, so keep retrying until it latches.
        context.coordinator.attach()
        context.coordinator.refreshTitle()
    }

    @MainActor
    final class Coordinator {
        var parent: WindowAccessor
        private weak var window: NSWindow?
        // NotificationCenter.removeObserver is thread-safe; the tokens are
        // only touched from install (main actor) and deinit (nonisolated).
        nonisolated(unsafe) private var observers: [NSObjectProtocol] = []

        init(_ parent: WindowAccessor) {
            self.parent = parent
        }

        func attach() {
            guard window == nil else { return }
            let candidate = NSApp.keyWindow
                ?? NSApp.windows.first { $0.isVisible && $0.tabbingIdentifier == "site.jrtx.booklight.reader" }
                ?? NSApp.windows.first(where: \.isVisible)
            guard let window = candidate else { return }
            self.window = window
            // Native window tabs: share a tabbing identity so Window >
            // Merge All Windows works and the tab bar's + button offers the
            // welcome state, exactly like Preview's document windows.
            window.tabbingMode = .preferred
            window.tabbingIdentifier = "site.jrtx.booklight.reader"
            let center = NotificationCenter.default
            observers.append(center.addObserver(
                forName: NSWindow.didBecomeKeyNotification, object: window, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.parent.app.activeReader = self.parent.reader
                }
            })
            observers.append(center.addObserver(
                forName: NSWindow.willCloseNotification, object: window, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.parent.reader.closeDocument()
                    self.parent.app.unregister(self.parent.reader)
                    // Books semantics: closing the last book returns to the
                    // shelf — the app never idles as a bare Dock icon.
                    let anyRemaining = NSApp.windows.contains {
                        $0.isVisible && $0 !== window
                            && $0.tabbingIdentifier == "site.jrtx.booklight.reader"
                    }
                    if !anyRemaining {
                        self.parent.app.openWindow?(nil)
                    }
                }
            })
            refreshTitle()
        }

        func refreshTitle() {
            // SwiftUI's navigationTitle owns window.title; this only keeps
            // the proxy icon (drag to Finder/Dock) in step with the document.
            guard let window else { return }
            if let path = parent.reader.documentPath {
                let url = URL(fileURLWithPath: path)
                if window.representedURL != url {
                    window.representedURL = url
                }
            } else if window.representedURL != nil {
                window.representedURL = nil
            }
        }

        deinit {
            for observer in observers { NotificationCenter.default.removeObserver(observer) }
        }
    }
}

// MARK: - reader window

struct ReaderWindowView: View {
    @EnvironmentObject private var app: AppModel
    @ObservedObject var reader: ReaderModel
    @Environment(\.openWindow) private var openWindowEnv
    @State private var sidebarVisibility: NavigationSplitViewVisibility = .automatic
    @State private var isKeyWindow = false

    /// Library-first: a window without a document IS the shelf; a window
    /// with one is a reader. Shared window plumbing wraps both modes.
    var body: some View {
        Group {
            if reader.hasDocument {
                readerChrome
            } else {
                LibraryView()
            }
        }
        .background(WindowAccessor(reader: reader, app: app))
        .overlay(alignment: .top) {
            if !app.status.isEmpty && !app.ready {
                backendBanner
            }
        }
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .dropDestination(for: URL.self) { urls, _ in
            dropOpened(urls)
            return true
        }
        .alert(reader.alertText, isPresented: $reader.showAlert) {}
        .sheet(item: $reader.passwordPrompt) { prompt in
            PasswordSheet(reader: reader, wrong: prompt.wrong)
                .interactiveDismissDisabled()
        }
        // Updater sheet only in the key window, so it never ghosts into
        // every open document at once.
        .sheet(isPresented: Binding(
            get: { isKeyWindow && app.showUpdateSheet },
            set: { app.showUpdateSheet = $0 })) {
            UpdateView()
        }
        .sheet(isPresented: $reader.showWatermark) {
            WatermarkSheet(reader: reader)
        }
        .onChange(of: reader.sidebarMode) { _ in
            // Picking a view — from the toolbar picker or the Tools menu —
            // while the sidebar is collapsed opens it.
            if sidebarVisibility == .detailOnly {
                sidebarVisibility = .all
            }
        }
        .onReceive(NotificationCenter.default.publisher(
            for: NSWindow.didBecomeKeyNotification)) { notification in
            isKeyWindow = (notification.object as? NSWindow) === NSApp.keyWindow
        }
        .onAppear {
            app.openWindow = { value in
                if let value {
                    openWindowEnv(value: value)
                } else {
                    openWindowEnv(id: "reader")
                }
            }
        }
    }

    /// The reading mode: native split sidebar + document + AI inspector.
    private var readerChrome: some View {
        NavigationSplitView(columnVisibility: $sidebarVisibility) {
            ReaderSidebar(reader: reader)
                .navigationSplitViewColumnWidth(min: 180, ideal: 232, max: 440)
        } detail: {
            detail
        }
        .inspector(isPresented: $reader.aiVisible) {
            AISidebar(reader: reader)
        }
        // The SwiftUI-owned window title (macOS: navigationTitle drives the
        // title bar and native tab labels).
        .navigationTitle(reader.documentTitle ?? RivetGeneratedConfig.displayName)
        .toolbar { readerToolbar }
        .environmentObject(reader)
        .overlay {
            if reader.findVisible {
                FindBarView(reader: reader)
                    .padding(.top, 4)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(.easeInOut(duration: 0.15), value: reader.findVisible)
    }

    private func dropOpened(_ urls: [URL]) {
        let dropped = urls
            .filter { ["pdf", "epub"].contains($0.pathExtension.lowercased()) }
        guard !dropped.isEmpty else { return }
        if !reader.hasDocument {
            // The shelf takes the drop: import all, open none.
            app.importBooks(dropped)
        } else {
            // Reading window: dropped books open in new windows.
            for url in dropped { app.openWindow?(url.path) }
        }
    }

    private var backendBanner: some View {
        Text(app.status)
            .font(.footnote)
            .foregroundStyle(Theme.danger)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 4)
            .background(Theme.accentSoft)
    }

    @ViewBuilder
    private var detail: some View {
        if reader.epubActive, let epub = reader.epub {
            EpubViewport(tab: epub)
                .ignoresSafeArea()
        } else if let tab = reader.pdf {
            CenteredDocument(tab: tab)
        }
    }

    @ToolbarContentBuilder
    private var readerToolbar: some ToolbarContent {
        // Sidebar view switcher in the Preview position: next to the
        // sidebar toggle, never bolted onto the sidebar itself. Picking a
        // mode while the sidebar is collapsed opens it.
        ToolbarItem(placement: .navigation) {
            Picker("", selection: $reader.sidebarMode) {
                if reader.epubActive {
                    Text(L10n.t("ui.viewer.outline")).tag(1)
                    Text(L10n.t("ui.sidebar.annotations")).tag(2)
                } else {
                    Text(L10n.t("ui.viewer.thumbs")).tag(0)
                    Text(L10n.t("ui.viewer.outline")).tag(1)
                    Text(L10n.t("ui.sidebar.annotations")).tag(2)
                    Text(L10n.t("ui.sidebar.forms")).tag(3)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .disabled(reader.pdf == nil && reader.epub == nil)
            .help(L10n.t("ui.toolbar.panel"))
        }

        ToolbarItemGroup(placement: .primaryAction) {
            if let epub = reader.epub, reader.epubActive {
                PageNavControl(
                    current: epub.currentPage, total: max(epub.chapters, 1),
                    jump: { epub.goToChapter($0) })
            } else if let tab = reader.pdf {
                PageNavControl(
                    current: tab.currentPage, total: tab.pageCount,
                    jump: { tab.goToPage($0) })
                zoomMenu(tab)
                toolbarIcon("rectangle.split.2x1", active: tab.splitOn,
                            help: L10n.t("ui.toolbar.split")) {
                    reader.toggleSplit()
                }
            }
            toolbarIcon("magnifyingglass", active: reader.findVisible,
                        help: L10n.t("ui.toolbar.find")) {
                if reader.findVisible {
                    reader.closeFind()
                } else if reader.hasDocument {
                    reader.findVisible = true
                }
            }
            .disabled(!reader.hasDocument)
            if let tab = reader.pdf {
                toolbarIcon(
                    tab.isBookmarked ? "bookmark.fill" : "bookmark",
                    active: tab.isBookmarked,
                    help: L10n.t("ui.toolbar.bookmark")) {
                    reader.toggleBookmark()
                }
            }
            toolbarIcon("speaker.wave.2", active: reader.ttsSpeaking,
                        help: L10n.t("ui.toolbar.tts")) {
                reader.toggleTTS()
            }
            .disabled(!reader.hasDocument)
            toolbarIcon("printer", active: false, help: L10n.t("ui.menu.print")) {
                reader.printDocument()
            }
            .disabled(reader.pdf == nil)
            toolbarIcon("sparkles", active: reader.aiVisible,
                        help: L10n.t("ui.toolbar.ai")) {
                reader.aiVisible.toggle()
            }
        }
    }

    /// Zoom cluster in the Preview form: a percent label that opens the zoom
    /// menu, sitting between the page control and the view toggles.
    @ViewBuilder
    private func zoomMenu(_ tab: PDFTab) -> some View {
        Menu {
            Button(L10n.t("ui.menu.zoom-in")) { HostActions.zoomIn?() }
                .keyboardShortcut("+", modifiers: .command)
            Button(L10n.t("ui.menu.zoom-out")) { HostActions.zoomOut?() }
                .keyboardShortcut("-", modifiers: .command)
            Divider()
            Button(L10n.t("ui.menu.actual-size")) { HostActions.zoomReset?() }
            Button(L10n.t("ui.viewer.fit")) { HostActions.fitWidth?() }
        } label: {
            Text(verbatim: "\(tab.scalePercent)%")
                .font(.system(size: 12, weight: .medium).monospacedDigit())
                .foregroundStyle(Theme.textDim)
                .frame(minWidth: 40)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.visible)
        .fixedSize()
    }

    private func toolbarIcon(_ symbol: String, active: Bool, help: String,
                             action: @escaping () -> Void) -> some View {
        Button {
            action()
        } label: {
            Image(systemName: symbol)
                .font(.system(size: 13))
                .frame(width: 26, height: 24)
        }
        .buttonStyle(.plain)
        .foregroundStyle(active ? Theme.accent : Theme.textDim)
        .accessibilityLabel(Text(verbatim: help))
        .help(help)
    }
}

/// Toolbar page navigation in the Preview form: ‹ [page] / N ›.
struct PageNavControl: View {
    let current: Int
    let total: Int
    let jump: (Int) -> Void
    @State private var pageInput = "1"
    @FocusState private var fieldFocused: Bool

    var body: some View {
        HStack(spacing: 2) {
            Button {
                jump(max(1, current - 1))
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 11, weight: .semibold))
            }
            .buttonStyle(.plain)
            .foregroundStyle(current > 1 ? Theme.textDim : Theme.textFaint)
            .disabled(current <= 1)
            TextField("", text: $pageInput)
                .textFieldStyle(.plain)
                .multilineTextAlignment(.center)
                .font(.system(size: 12).monospacedDigit())
                .frame(width: 30)
                .focused($fieldFocused)
                .onSubmit(submit)
            Text(verbatim: "/ \(total)")
                .font(.system(size: 12).monospacedDigit())
                .foregroundStyle(Theme.textFaint)
            Button {
                jump(min(total, current + 1))
            } label: {
                Image(systemName: "chevron.right")
                    .font(.system(size: 11, weight: .semibold))
            }
            .buttonStyle(.plain)
            .foregroundStyle(current < total ? Theme.textDim : Theme.textFaint)
            .disabled(current >= total)
        }
        .onAppear { pageInput = "\(current)" }
        .onChange(of: current) { pageInput = "\($0)" }
    }

    private func submit() {
        guard let page = Int(pageInput), page >= 1, page <= total else {
            pageInput = "\(current)"
            return
        }
        jump(page)
        fieldFocused = false
    }
}

/// Document area. Observes the tab directly so tab-level state (splitOn)
/// re-renders it — the environment model alone does not.
private struct CenteredDocument: View {
    @ObservedObject var tab: PDFTab

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            PDFViewport(tab: tab)
                .ignoresSafeArea()
                .background(Theme.bg)
            if tab.splitOn {
                Divider().overlay(Theme.border)
                SplitViewport(tab: tab)
                    .ignoresSafeArea()
                    .background(Theme.panelAlt)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - find bar

struct FindBarView: View {
    @ObservedObject var reader: ReaderModel

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(Theme.textDim)
            TextField(L10n.t("ui.viewer.find-placeholder"), text: Binding(
                get: { reader.findQuery },
                set: { reader.findQuery = $0; reader.runFind() }))
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .frame(width: 180)
                .onSubmit { reader.findNext() }
            if reader.findTotal > 0 {
                Text("\(reader.findIndex + 1)/\(reader.findTotal)")
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textDim)
            } else if !reader.findQuery.isEmpty {
                Text("0")
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textFaint)
            }
            Button {
                reader.findPrevious()
            } label: {
                Image(systemName: "chevron.up")
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.textDim)
            Button {
                reader.findNext()
            } label: {
                Image(systemName: "chevron.down")
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.textDim)
            Button {
                reader.closeFind()
            } label: {
                Image(systemName: "xmark")
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.textDim)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.borderStrong))
        .shadow(color: Color.black.opacity(0.18), radius: 8, y: 2)
    }
}

// MARK: - password sheet

struct PasswordSheet: View {
    @ObservedObject var reader: ReaderModel
    let wrong: Bool
    @State private var password = ""

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "lock")
                .font(.system(size: 26))
                .foregroundStyle(Theme.accent)
            Text(L10n.t("ui.password.title"))
                .font(.system(size: 14, weight: .semibold))
            if wrong {
                Text(L10n.t("ui.password.wrong"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.danger)
            }
            SecureField("", text: $password)
                .textFieldStyle(.roundedBorder)
                .frame(width: 240)
                .onSubmit { submit() }
            HStack(spacing: 10) {
                Button(L10n.t("ui.password.cancel")) {
                    reader.cancelPassword()
                }
                .keyboardShortcut(.cancelAction)
                Button(L10n.t("ui.password.ok")) { submit() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(password.isEmpty)
            }
        }
        .padding(24)
        .frame(width: 320)
    }

    private func submit() {
        guard !password.isEmpty else { return }
        reader.submitPassword(password)
        password = ""
    }
}


// MARK: - watermark sheet (Tools > Add Watermark)

struct WatermarkSheet: View {
    @ObservedObject var reader: ReaderModel
    @Environment(\.dismiss) private var dismiss
    @State private var text = "机密"
    @State private var size = 48
    @State private var opacityPercent = 30

    var body: some View {
        VStack(spacing: 14) {
            Text(L10n.t("ui.tools.watermark"))
                .font(.system(size: 14, weight: .semibold))
            TextField(L10n.t("ui.watermark.text"), text: $text)
                .textFieldStyle(.roundedBorder)
                .frame(width: 260)
            HStack(spacing: 10) {
                Text(L10n.t("ui.watermark.size"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textDim)
                Stepper("\(size)", value: $size, in: 12...144)
                    .font(.system(size: 12))
                Spacer()
            }
            .frame(width: 260)
            HStack(spacing: 10) {
                Text(L10n.t("ui.watermark.opacity"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textDim)
                Slider(value: Binding(
                    get: { Double(opacityPercent) },
                    set: { opacityPercent = Int($0) }), in: 5...100)
                Text("\(opacityPercent)%")
                    .font(.system(size: 12).monospacedDigit())
                    .frame(width: 40)
            }
            .frame(width: 260)
            HStack(spacing: 10) {
                Button(L10n.t("ui.password.cancel")) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(L10n.t("ui.watermark.apply")) {
                    dismiss()
                    reader.applyWatermark(WatermarkDraft(
                        text: text,
                        size: size,
                        opacityPercent: opacityPercent))
                }
                .keyboardShortcut(.defaultAction)
                .disabled(text.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(24)
        .frame(width: 340)
        .onAppear {
            if let draft = reader.watermarkDraft {
                text = draft.text
                size = draft.size
                opacityPercent = draft.opacityPercent
            }
        }
    }
}
