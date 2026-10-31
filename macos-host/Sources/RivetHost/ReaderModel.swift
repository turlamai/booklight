import SwiftUI
import AppKit
import AVFoundation
import UniformTypeIdentifiers
import PDFKit
import WebKit
import RivetRuntime

enum AnnotationPersistenceState: Equatable {
    case saved
    case pending
    case recovered
    case failed(String)
}

/// One open PDF document inside one reader window. Owns exactly one PDFView
/// (plus a split mirror) so selection, zoom and scroll survive for the
/// window's lifetime; business actions live in ReaderModel.
@MainActor
final class PDFTab: ObservableObject, Identifiable {
    let id = UUID()
    let path: String
    let title: String
    let pdfView: GistPDFView
    let splitPdfView: GistPDFView
    weak var reader: ReaderModel?

    @Published var pageCount = 0
    @Published var currentPage = 1
    @Published var scalePercent = 100
    @Published var isBookmarked = false
    @Published var docData = DocData.empty
    @Published var outline: [OutlineNode] = []
    @Published var splitOn = false

    var resumePage = 1
    var didInitialLayout = false
    var recentTask: Task<Void, Never>?
    var annoTask: Task<Void, Never>?
    var annotationsLoaded = false

    init(path: String, title: String, document: PDFDocument, reader: ReaderModel) {
        self.path = path
        self.title = title
        self.reader = reader
        let view = GistPDFView()
        view.minScaleFactor = 0.4
        view.maxScaleFactor = 4.0
        view.autoScales = false
        view.displayMode = reader.app?.settings.view_mode == "double" ? .twoUp : .singlePage
        view.displaysPageBreaks = true
        view.document = document
        self.pdfView = view
        // Split mirror (v1 viewer-split): shares the document, has no
        // toolbar state of its own, only follows the main view's scroll.
        let split = GistPDFView()
        split.minScaleFactor = 0.4
        split.maxScaleFactor = 4.0
        split.autoScales = false
        split.displayMode = view.displayMode
        split.displaysPageBreaks = true
        split.document = document
        self.splitPdfView = split
        view.hostTab = self
        self.pageCount = document.pageCount
    }

    var document: PDFDocument? { pdfView.document }

    /// Reading-theme page canvas behind the pages (paper/sepia/night); the
    /// page content inversion is coordinated by the ReaderModel (it owns
    /// the stored annotations that must survive the document swap).
    func applyCanvasSurface(_ surface: String) {
        let color = NSColor(hex: surface == "dark" ? 0x171412
                            : surface == "sepia" ? 0xEFE6D5 : 0xF5F2ED)
        pdfView.backgroundColor = color
        splitPdfView.backgroundColor = color
    }

    func goToPage(_ page: Int) {
        guard let doc = document, page >= 1, page <= doc.pageCount,
              let target = doc.page(at: page - 1) else { return }
        let top = NSPoint(x: 0, y: target.bounds(for: .mediaBox).maxY)
        pdfView.go(to: PDFDestination(page: target, at: top))
        currentPage = page
    }

    func zoomIn() {
        if let epub = reader?.epub, reader?.epubActive == true {
            let next = min(2.0, epub.fontScale * 1.1)
            epub.applyFontScale(next)
            reader?.app?.saveReadingFontScale(next)
            return
        }
        pdfView.scaleFactor = min(pdfView.scaleFactor * 1.2, pdfView.maxScaleFactor)
    }

    func zoomOut() {
        if let epub = reader?.epub, reader?.epubActive == true {
            let next = max(0.7, epub.fontScale / 1.1)
            epub.applyFontScale(next)
            reader?.app?.saveReadingFontScale(next)
            return
        }
        pdfView.scaleFactor = max(pdfView.scaleFactor / 1.2, pdfView.minScaleFactor)
    }

    func zoomReset() {
        if let epub = reader?.epub, reader?.epubActive == true {
            epub.applyFontScale(1.0)
            reader?.app?.saveReadingFontScale(1.0)
            return
        }
        pdfView.scaleFactor = 1.0
    }

    func fitWidth() {
        guard let page = pdfView.currentPage ?? document?.page(at: 0) else { return }
        let bounds = page.bounds(for: .mediaBox)
        guard bounds.width > 0, pdfView.bounds.width > 0 else { return }
        let target = (pdfView.bounds.width - 32) / bounds.width
        pdfView.scaleFactor = min(max(target, pdfView.minScaleFactor), pdfView.maxScaleFactor)
    }

    func setDisplayMode(double: Bool) {
        pdfView.displayMode = double ? .twoUp : .singlePage
    }

    func refreshBookmarkFlag() {
        isBookmarked = docData.bookmarks.contains { $0.page == currentPage }
    }

    // MARK: context menu (right-click on a selection)

    func selectionMenu() -> NSMenu {
        let menu = NSMenu()
        menu.autoenablesItems = false
        menu.addItem(BlockMenuItem(title: L10n.t("ui.menu.translate")) { [weak self] in
            self?.reader?.translateSelection()
        })
        menu.addItem(.separator())
        let highlightColors: [(String, String)] = [
            ("yellow", "ui.menu.highlight-yellow"),
            ("green", "ui.menu.highlight-green"),
            ("blue", "ui.menu.highlight-blue"),
        ]
        for (color, key) in highlightColors {
            menu.addItem(BlockMenuItem(title: L10n.t(key)) { [weak self] in
                self?.reader?.addAnnotationFromSelection(color: color, kind: "highlight")
            })
        }
        menu.addItem(BlockMenuItem(title: L10n.t("ui.menu.underline")) { [weak self] in
            self?.reader?.addAnnotationFromSelection(color: "blue", kind: "underline")
        })
        menu.addItem(BlockMenuItem(title: L10n.t("ui.menu.strike")) { [weak self] in
            self?.reader?.addAnnotationFromSelection(color: "yellow", kind: "strike")
        })
        menu.addItem(.separator())
        let copy = NSMenuItem(
            title: L10n.t("ui.menu.copy"),
            action: #selector(PDFView.copy(_:)), keyEquivalent: "c")
        copy.target = pdfView
        copy.isEnabled = true
        menu.addItem(copy)
        return menu
    }
}

/// Per reader window: the open document (PDF or EPUB, or none = welcome
/// state) plus everything the document commands — find, AI, annotations,
/// page surgery, TTS, printing. The process-wide backend and settings live
/// in the shared AppModel.
@MainActor
final class ReaderModel: ObservableObject {
    private(set) weak var app: AppModel?

    @Published var pdf: PDFTab?
    @Published var epub: EpubTab?
    @Published var epubActive = false

    /// Sidebar navigation (pure current-document: thumbs / outline / notes).
    @Published var sidebarMode = 0  // 0 thumbs / 1 outline / 2 annotations

    @Published var aiVisible = false
    @Published var aiTab: AITab = .translate
    @Published var translateCards: [StreamCard] = []
    @Published var summarizeCards: [StreamCard] = []
    @Published var chatBubbles: [ChatBubble] = []
    @Published var chatBusy = false
    @Published var chatScope = 0  // 0 page / 1 selection / 2 doc
    @Published var findVisible = false
    @Published var findQuery = ""
    @Published var findTotal = 0
    @Published var findIndex = 0
    @Published var ttsSpeaking = false
    @Published var showAlert = false
    @Published var alertText = ""
    @Published var passwordPrompt: PasswordPrompt?
    /// Per-window persistence state. Keeping this on ReaderModel rather than
    /// AppModel prevents one document's save result from appearing in another
    /// native window.
    @Published var annotationPersistence: AnnotationPersistenceState = .saved

    private var annotationRevision: UInt64 = 0

    private var chatHistory: [[String]] = []
    private var chatLive: StreamCard?
    private var chatPendingQuestion = ""
    private var stoppingStreams: Set<Int64> = []
    private var findTask: Task<Void, Never>?
    private var findHits: [PDFSelection] = []
    // EPUB whole-book search results (chapter, ordinal) — v1 EpubHit
    private var epubFindHits: [(chapter: Int, ordinal: Int)] = []
    private var epubFindQuery = ""
    private var epubFindHitIndex = 0

    private let synthesizer = AVSpeechSynthesizer()
    private lazy var ttsDelegate = TTSDelegate()

    init(app: AppModel) {
        self.app = app
    }

    var hasDocument: Bool { pdf != nil || epub != nil }

    var documentPath: String? { epubActive ? epub?.path : pdf?.path }

    var documentTitle: String? {
        if epubActive, let epub { return epub.title }
        return pdf?.title
    }

    func alert(_ message: String) {
        alertText = message
        showAlert = true
    }

    // MARK: open

    func openDropped(_ urls: [URL]) {
        guard let url = urls.first(where: { ["pdf", "epub"].contains($0.pathExtension.lowercased()) }) else { return }
        Task { await openPath(url.path) }
    }

    /// Open into this window. The window only ever holds one document, so an
    /// occupied window is either showing the same file (jump to the resume
    /// page) or the caller should have opened a new window instead.
    func openPath(_ path: String, resumePage: Int? = nil) async {
        if path.lowercased().hasSuffix(".epub") {
            await openEpub(path, resumeChapter: resumePage ?? app?.recentPage(path) ?? 1)
            return
        }
        if let existing = pdf, existing.path == path {
            if let page = resumePage ?? app?.recentPage(path), page > 1 { existing.goToPage(page) }
            return
        }
        guard pdf == nil && epub == nil else { return }
        guard let doc = InvertedPDFDocument(url: URL(fileURLWithPath: path)) else {
            alert(L10n.t("ui.open-failed", path))
            return
        }
        var wrong = false
        while doc.isLocked {
            let password = await requestPassword(wrong: wrong)
            guard let password, !password.isEmpty else { return }
            wrong = true
            if doc.unlock(withPassword: password) { break }
        }
        let title = URL(fileURLWithPath: path).lastPathComponent
        let tab = PDFTab(path: path, title: title, document: doc, reader: self)
        annotationRevision = 0
        annotationPersistence = .saved
        tab.resumePage = max(resumePage ?? app?.recentPage(path) ?? 1, 1)
        let surface = app?.readingSurface() ?? "light"
        tab.applyCanvasSurface(surface)
        InvertedPDFPage.isInverted = (surface == "dark")
        pdf = tab
        epubActive = false
        sidebarMode = 0
        loadDocData(tab)
        noteActivity(tab, immediate: true)
        app?.importOpenedBook(path: path, doc: doc)
    }

    // MARK: EPUB (one book per window)

    func openEpub(_ path: String, resumeChapter: Int = 1) async {
        if let existing = epub, existing.path == path {
            epubActive = true
            if resumeChapter > 1 { existing.goToChapter(resumeChapter) }
            return
        }
        guard pdf == nil && epub == nil else { return }
        guard let api = app?.api else {
            alert(L10n.t("ui.backend-not-ready"))
            return
        }
        let title = URL(fileURLWithPath: path).lastPathComponent
        let config = WKWebViewConfiguration()
        let view = WKWebView(frame: .zero, configuration: config)
        view.setValue(false, forKey: "drawsBackground")
        let tab = EpubTab(path: path, title: title, view: view, reader: self)
        annotationRevision = 0
        annotationPersistence = .saved
        // Persisted typography: the stored scale rides in until the page
        // finishes loading (didFinish pushes it into the CSS variable).
        tab.fontScale = Double(app?.settings.reading_font_scale_permille ?? 1000) / 1000.0
        epub = tab
        epubActive = true
        pdf = nil
        sidebarMode = 1
        findVisible = false
        tab.loadBook(api: api)
        tab.loadAnnotations(api: api, sidecar: app?.settings.annotation_sidecar ?? false)
        if resumeChapter > 1 {
            tab.currentPage = resumeChapter
        }
        noteEpubActivity(tab, immediate: true)
        app?.importOpenedEpub(path: path)
    }

    func noteEpubChapter(_ tab: EpubTab) {
        noteEpubActivity(tab)
    }

    // MARK: window lifecycle

    /// Reading-theme re-skin from the settings pane: EPUB page theme now,
    /// PDF page canvas in both panes — and on a night flip, the page
    /// content inversion (Books parity; the Windows and Linux hosts invert
    /// inside their PDFium pipelines — PDFKit has no render hook, so the
    /// flip re-creates the document from the same file).
    func applyReadingSurface(_ surface: String) {
        epub?.applyTheme(surface)
        pdf?.applyCanvasSurface(surface)
        applyInversion(surface)
    }

    private func applyInversion(_ surface: String) {
        guard let tab = pdf else { return }
        let inverted = surface == "dark"
        guard inverted != InvertedPDFPage.isInverted else { return }
        InvertedPDFPage.isInverted = inverted
        // Fresh pages draw with the current flag; same file, position,
        // zoom, mode and stored annotations restored.
        guard let doc = InvertedPDFDocument(url: URL(fileURLWithPath: tab.path)),
              !doc.isLocked else { return }
        let page = tab.currentPage
        let scale = tab.pdfView.scaleFactor
        let mode = tab.pdfView.displayMode
        tab.pdfView.document = doc
        tab.splitPdfView.document = doc
        tab.pageCount = doc.pageCount
        applyStoredAnnotations(tab)
        rebuildOutline(tab)
        tab.refreshBookmarkFlag()
        tab.pdfView.displayMode = mode
        tab.goToPage(page)
        tab.pdfView.scaleFactor = scale
    }

    /// Called when the window closes: flush pending persistence, stop live
    /// streams, tear the web view down.
    func closeDocument() {
        if let tab = pdf {
            tab.recentTask?.cancel()
            tab.annoTask?.cancel()
            saveRecentNow(tab)
        }
        if let epub = epub {
            epub.recentTask?.cancel()
            epub.annoTask?.cancel()
            saveEpubRecentNow(epub)
            epub.teardown()
        }
        let live = (translateCards + summarizeCards + chatBubbles.compactMap { $0.card })
            .map { $0.requestId }
            .filter { $0 > 0 }
        app?.stopStreams(live)
    }

    /// The application delegate uses macOS terminate-later semantics and
    /// awaits this handoff before allowing process exit. Window close remains
    /// non-blocking because the shared backend stays alive on the shelf.
    func flushAnnotationsBeforeTermination() async {
        if let tab = pdf, tab.annotationsLoaded || annotationRevision > 0 {
            tab.annoTask?.cancel()
            await storeAnnotationData(tab, revision: annotationRevision)
        }
        if let tab = epub, tab.annotationsLoaded || annotationRevision > 0 {
            tab.annoTask?.cancel()
            await storeEpubAnnotationData(tab)
        }
    }

    // MARK: page navigation (Go menu / toolbar)

    func goPage(_ step: Int) {
        if epubActive, let epub = epub {
            epub.goToChapter(max(1, min(epub.currentPage + step, max(epub.chapters, 1))))
            return
        }
        guard let tab = pdf else { return }
        tab.goToPage(max(1, min(tab.currentPage + step, tab.pageCount)))
    }

    func goToEdgePage(last: Bool) {
        if epubActive, let epub = epub {
            epub.goToChapter(last ? max(epub.chapters, 1) : 1)
            return
        }
        guard let tab = pdf else { return }
        tab.goToPage(last ? tab.pageCount : 1)
    }

    func toggleSplit() {
        pdf?.splitOn.toggle()
    }

    /// Tools > Merge PDF: pick the second document, then hand both paths to
    /// the domain core (result replaces the in-memory document).
    func pickAndMergePDF() {
        guard pdf != nil else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.pdf]
        panel.canChooseFiles = true
        panel.message = L10n.t("ui.pages.merge")
        let handle: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.runPageEdit(.merge(path: url.path))
        }
        if let window = NSApp.keyWindow {
            panel.beginSheetModal(for: window, completionHandler: handle)
        } else {
            handle(panel.runModal())
        }
    }

    /// Tools > Fill Form: the forms pane is a sidebar view (document tools
    /// live on the document side; the right inspector is AI-only).
    func showForms() {
        guard pdf != nil else { return }
        sidebarMode = 3
    }

    /// Tools > Add Watermark: sheet fields, then edit-bake-text (in-memory).
    @Published var showWatermark = false
    @Published var watermarkDraft: WatermarkDraft?

    func showWatermarkDialog() {
        guard pdf != nil else { return }
        watermarkDraft = WatermarkDraft()
    }

    func applyWatermark(_ draft: WatermarkDraft) {
        guard let tab = pdf, let api = app?.api else { return }
        watermarkDraft = nil
        Task { [weak self] in
            do {
                let data = try await api.edit_bake_text(
                    path: tab.path,
                    watermark_text: draft.text,
                    watermark_size: Int64(draft.size),
                    watermark_opacity_milli: Int64(draft.opacityPercent) * 1000,
                    boxes: [])
                await MainActor.run { self?.reloadTabDocument(tab, data: data) }
            } catch {
                await MainActor.run {
                    self?.alert(L10n.t("ui.pages.edit-failed", AppModel.cleanError(error)))
                }
            }
        }
    }

    /// Tools > Export Current Page as Image: the visible page at 2x.
    func exportCurrentPageImage() {
        guard let tab = pdf, let page = tab.pdfView.currentPage else { return }
        let bounds = page.bounds(for: .mediaBox)
        let scale: CGFloat = 2
        let thumb = page.thumbnail(
            of: CGSize(width: bounds.width * scale, height: bounds.height * scale),
            for: .mediaBox)
        guard let tiff = thumb.tiffRepresentation,
              let rep = NSBitmapImageRep(data: tiff),
              let png = rep.representation(using: .png, properties: [:]) else {
            alert(L10n.t("ui.export.failed", "render"))
            return
        }
        let stem = (tab.title as NSString).deletingPathExtension
        saveData(png, defaultName: "\(stem)-\(tab.currentPage).png", type: .png)
    }

    /// Notes sidebar export: the digest renders backend-side over exactly
    /// the bytes this tab holds (unsaved edits included — no flush race).
    func exportAnnotations() {
        guard let tab = pdf else { return }
        Task {
            guard let app, let api = app.api else { return }
            guard let data = try? JSONEncoder().encode(tab.docData) else { return }
            do {
                let markdown = try await api.export_annotations(
                    data: data, title: tab.title)
                let stem = (tab.title as NSString).deletingPathExtension
                let name = stem + L10n.t("ui.notes.export-suffix") + ".md"
                saveData(Data(markdown.utf8), defaultName: name, type: .plainText)
            } catch let e as ClientError {
                if case .backend(let message) = e {
                    alert(L10n.t("ui.pages.edit-failed", message))
                } else {
                    alert(L10n.t("ui.pages.edit-failed", String(describing: e)))
                }
            } catch {
                alert(L10n.t("ui.pages.edit-failed", String(describing: error)))
            }
        }
    }

    /// Tools > Export Text: the whole document's text layer as .txt.
    func exportDocumentText() {
        guard let tab = pdf, let doc = tab.document else { return }
        guard let text = doc.string?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty else {
            alert(L10n.t("ui.need.text"))
            return
        }
        let stem = (tab.title as NSString).deletingPathExtension
        saveData(Data(text.utf8), defaultName: stem + ".txt", type: .plainText)
    }

    /// Shared typed save panel (page image / text export).
    func saveData(_ data: Data, defaultName: String, type: UTType) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [type]
        panel.nameFieldStringValue = defaultName
        let handle: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            do {
                try data.write(to: url)
            } catch {
                self?.alert(L10n.t("ui.export.failed", error.localizedDescription))
            }
        }
        if let window = NSApp.keyWindow {
            panel.beginSheetModal(for: window, completionHandler: handle)
        } else {
            handle(panel.runModal())
        }
    }

    func openFind() {
        if epubActive {
            if epub != nil { findVisible = true }
            return
        }
        guard pdf != nil else { return }
        findVisible = true
    }

    func selectionCleared() {}

    // MARK: print

    func printDocument() {
        guard let doc = pdf?.document,
              let op = doc.printOperation(for: .shared, scalingMode: .pageScaleDownToFit, autoRotate: true)
        else { return }
        op.run()
    }

    // MARK: page-level editing (backend owns the PDF surgery — issue #1)

    func runPageEdit(_ op: PageEditOp) {
        Task { await runPageEditAsync(op) }
    }

    private func runPageEditAsync(_ op: PageEditOp) async {
        guard let tab = pdf, let api = app?.api else { return }
        let page = Int64(tab.currentPage)
        do {
            let data: Data
            switch op {
            case .deleteCurrent:
                data = try await api.edit_delete_pages(path: tab.path, pages: [page])
            case .rotateCurrent:
                data = try await api.edit_rotate_pages(path: tab.path, pages: [page], delta: 90)
            case .insertBlankAfterCurrent:
                data = try await api.edit_insert_blank_after(path: tab.path, page: page)
            case .extractCurrent:
                let bytes = try await api.edit_extract_pages(path: tab.path, pages: [page])
                try await saveEditResult(bytes,
                                         defaultName: extractName(tab.title))
                return
            case .extract(let pages):
                let bytes = try await api.edit_extract_pages(path: tab.path, pages: pages)
                try await saveEditResult(bytes,
                                         defaultName: extractName(tab.title))
                return
            case .merge(let otherPath):
                data = try await api.edit_append_doc(path: tab.path, other_path: otherPath)
            }
            reloadTabDocument(tab, data: data)
        } catch let e as ClientError {
            if case .backend(let message) = e {
                alert(L10n.t("ui.pages.edit-failed", message))
            } else {
                alert(L10n.t("ui.pages.edit-failed", String(describing: e)))
            }
        } catch {
            alert(L10n.t("ui.pages.edit-failed", String(describing: error)))
        }
    }

    /// Replace the tab's in-memory document (v1 pdf.reloadBytes parity):
    /// same tab, selection/scroll reset, source file untouched on disk.
    func reloadTabDocument(_ tab: PDFTab, data: Data) {
        guard let newDoc = PDFDocument(data: data) else {
            alert(L10n.t("ui.pages.edit-failed", "unreadable result"))
            return
        }
        tab.pdfView.document = newDoc
        tab.splitPdfView.document = newDoc
        tab.pageCount = newDoc.pageCount
        tab.goToPage(1)
        tab.refreshBookmarkFlag()
    }

    private func extractName(_ title: String) -> String {
        let stem = title.replacingOccurrences(
            of: "\\.pdf$", with: "", options: [.regularExpression, .caseInsensitive])
        return stem + L10n.t("ui.pages.extract-suffix") + ".pdf"
    }

    func saveEditResult(_ data: Data, defaultName: String) async {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.pdf]
        panel.nameFieldStringValue = defaultName
        let response = await panel.beginSheetModal(for: NSApp.mainWindow!)
        guard response == .OK, let url = panel.url else { return }
        do {
            try data.write(to: url)
        } catch {
            alert(L10n.t("ui.pages.save-failed", error.localizedDescription))
        }
    }

    // MARK: TTS (read the current page aloud — v1 main.ts toggleTTS)

    func toggleTTS() {
        if ttsSpeaking {
            synthesizer.stopSpeaking(at: .immediate)
            ttsSpeaking = false
            return
        }
        if epubActive, let epub = epub {
            let raw = epubTextSync(epub) // cache-filled by prefetch
            guard !raw.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                alert(L10n.t("ui.tts.empty"))
                return
            }
            speak(raw)
            return
        }
        guard let page = pdf?.pdfView.currentPage, let raw = page.string else { return }
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            alert(L10n.t("ui.tts.empty"))
            return
        }
        speak(text)
    }

    private func speak(_ text: String) {
        synthesizer.delegate = ttsDelegate
        ttsDelegate.reader = self
        let utterance = AVSpeechUtterance(string: String(text.prefix(20000)))
        utterance.voice = AVSpeechSynthesisVoice(language: "zh-CN")
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate
        synthesizer.speak(utterance)
        ttsSpeaking = true
    }

    // MARK: PDFKit callbacks (from PDFViewport coordinator)

    func pageChanged(_ tab: PDFTab) {
        if let page = tab.pdfView.currentPage, let doc = tab.pdfView.document {
            tab.currentPage = doc.index(for: page) + 1
        }
        tab.refreshBookmarkFlag()
        noteActivity(tab)
    }

    func scaleChanged(_ tab: PDFTab) {
        tab.scalePercent = Int(round(tab.pdfView.scaleFactor * 100))
    }

    // MARK: recents

    // EPUB recents ride the same update-recents RPC with page = chapter.
    func noteEpubActivity(_ tab: EpubTab, immediate: Bool = false) {
        tab.recentTask?.cancel()
        tab.recentTask = Task { [weak self] in
            if !immediate {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
            guard !Task.isCancelled else { return }
            await self?.saveEpubRecent(tab)
        }
    }

    private func saveEpubRecent(_ tab: EpubTab) async {
        guard let app else { return }
        await app.noteRecent(path: tab.path, page: tab.currentPage, of: tab.chapters)
    }

    private func saveEpubRecentNow(_ tab: EpubTab) {
        tab.recentTask?.cancel()
        Task { await saveEpubRecent(tab) }
    }

    func noteActivity(_ tab: PDFTab, immediate: Bool = false) {
        tab.recentTask?.cancel()
        tab.recentTask = Task { [weak self] in
            if !immediate {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
            guard !Task.isCancelled else { return }
            await self?.saveRecent(tab)
        }
    }

    private func saveRecent(_ tab: PDFTab) async {
        await app?.noteRecent(path: tab.path, page: tab.currentPage, of: tab.pageCount)
    }

    private func saveRecentNow(_ tab: PDFTab) {
        tab.recentTask?.cancel()
        Task { await saveRecent(tab) }
    }

    // MARK: document data (annotations + bookmarks)

    private func loadDocData(_ tab: PDFTab) {
        Task { [weak self] in
            guard let self, let app = self.app, let api = app.api else { return }
            if let data = try? await api.get_annotations(
                pdf_path: tab.path, sidecar: app.settings.annotation_sidecar),
               let decoded = try? JSONDecoder().decode(DocData.self, from: data) {
                tab.docData = decoded
                self.applyStoredAnnotations(tab)
                tab.refreshBookmarkFlag()
                self.rebuildOutline(tab)
                tab.annotationsLoaded = true
                let recovered = (try? await api.annotation_recovery_pending(
                    pdf_path: tab.path, sidecar: app.settings.annotation_sidecar)) ?? false
                self.annotationPersistence = recovered ? .recovered : .saved
            }
        }
    }

    private func applyStoredAnnotations(_ tab: PDFTab) {
        for annotation in tab.docData.annotations {
            renderAnnotation(annotation, in: tab)
        }
    }

    func renderAnnotation(_ annotation: StoredAnnotation, in tab: PDFTab) {
        guard let doc = tab.document, doc.pageCount >= annotation.page,
              let page = doc.page(at: annotation.page - 1) else { return }
        let type: PDFAnnotationSubtype
        switch annotation.kind {
        case "underline": type = .underline
        case "strike": type = .strikeOut
        default: type = .highlight
        }
        for rect in annotation.rects {
            let bounds = rect.nsRect
            guard bounds.width > 0, bounds.height > 0 else { continue }
            let ann = PDFAnnotation(bounds: bounds, forType: type, withProperties: nil)
            ann.color = Theme.annotationColor(annotation.color)
            ann.userName = "pdfgist:\(annotation.id)"
            page.addAnnotation(ann)
        }
    }

    func addAnnotationFromSelection(color: String, kind: String) {
        guard let tab = pdf, let selection = tab.pdfView.currentSelection,
              let text = selection.string?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty, let page = selection.pages.first,
              let doc = tab.document else { return }
        let pageNo = doc.index(for: page) + 1
        let rects = selection.selectionsByLine()
            .map { $0.bounds(for: page) }
            .filter { !$0.isEmpty }
            .map(AnnoRect.init)
        let id = String(format: "a-%lld-%04d", Int(Date().timeIntervalSince1970), Int.random(in: 0..<10_000))
        let annotation = StoredAnnotation(
            id: id, page: pageNo, rects: rects,
            excerpt: String(text.prefix(800)), color: color, kind: kind,
            note: "", created: Int(Date().timeIntervalSince1970))
        tab.docData.annotations.append(annotation)
        renderAnnotation(annotation, in: tab)
        queueAnnotationSave(tab)
    }

    func removeAnnotation(_ id: String) {
        guard let tab = pdf,
              let index = tab.docData.annotations.firstIndex(where: { $0.id == id })
        else { return }
        let removed = tab.docData.annotations.remove(at: index)
        guard let doc = tab.document else { return }
        for index in 0..<doc.pageCount {
            guard let page = doc.page(at: index) else { continue }
            for annotation in page.annotations
            where annotation.userName == "pdfgist:\(id)" {
                page.removeAnnotation(annotation)
            }
        }
        queueAnnotationSave(tab)
        stageUndo(of: removed, at: index, in: tab)
    }

    // MARK: one-step undo (taskly banner parity)

    /// The annotation the last delete removed, kept for the banner's
    /// five-second window. Deletion persists immediately; undo re-inserts
    /// at the old position and persists again.
    @Published var pendingUndo: StoredAnnotation?
    var pendingUndoIndex = 0
    var undoTask: Task<Void, Never>?

    /// EPUB twin of the one-step undo (annotation + its position).
    @Published var epubUndo: StoredAnnotation?
    var epubUndoIndex = 0
    var epubUndoTask: Task<Void, Never>?

    private func stageUndo(of annotation: StoredAnnotation, at index: Int, in tab: PDFTab) {
        pendingUndo = annotation
        pendingUndoIndex = index
        undoTask?.cancel()
        undoTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            guard !Task.isCancelled else { return }
            self?.pendingUndo = nil
        }
    }

    // MARK: EPUB annotations (chapter+offset anchored; marks live in the page)

    func addEpubAnnotation(color: String, kind: String) {
        guard let epub = epub, let selection = epub.pendingSelection else { return }
        let id = String(format: "e-%lld-%04d",
                        Int(Date().timeIntervalSince1970),
                        Int.random(in: 0..<10_000))
        let annotation = StoredAnnotation(
            id: id, page: selection.chapter, rects: [],
            excerpt: String(selection.text.prefix(800)), color: color,
            kind: kind, note: "", created: Int(Date().timeIntervalSince1970),
            anchor: StoredAnchor(chapter: selection.chapter,
                                 start: selection.start, end: selection.end))
        epub.docData.annotations.append(annotation)
        epub.webView.evaluateJavaScript(
            "applyEpubAnnotation(\(EpubTab.jsString(id)), \(selection.chapter), \(selection.start), \(selection.end), \(EpubTab.jsString(kind)), \(EpubTab.jsString(color)));",
            completionHandler: nil)
        epub.queueAnnotationSave()
    }

    /// EPUB digest export (same RPC; page ordinal = chapter).
    func exportEpubAnnotations() {
        guard let epub = epub else { return }
        Task {
            guard let app, let api = app.api else { return }
            guard let data = try? JSONEncoder().encode(epub.docData) else { return }
            do {
                let markdown = try await api.export_annotations(
                    data: data, title: epub.title)
                let name = epub.title + L10n.t("ui.notes.export-suffix") + ".md"
                saveData(Data(markdown.utf8), defaultName: name, type: .plainText)
            } catch let e as ClientError {
                if case .backend(let message) = e {
                    alert(L10n.t("ui.pages.edit-failed", message))
                } else {
                    alert(L10n.t("ui.pages.edit-failed", String(describing: e)))
                }
            } catch {
                alert(L10n.t("ui.pages.edit-failed", String(describing: error)))
            }
        }
    }

    func removeEpubAnnotation(_ id: String) {
        guard let epub = epub,
              let index = epub.docData.annotations.firstIndex(where: { $0.id == id })
        else { return }
        let removed = epub.docData.annotations.remove(at: index)
        epub.webView.evaluateJavaScript(
            "removeEpubAnnotation(\(EpubTab.jsString(id)));",
            completionHandler: nil)
        epub.queueAnnotationSave()
        epubUndo = removed
        epubUndoIndex = index
        epubUndoTask?.cancel()
        epubUndoTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            guard !Task.isCancelled else { return }
            self?.epubUndo = nil
        }
    }

    func undoDeleteEpubAnnotation() {
        guard let epub = epub, let annotation = epubUndo else { return }
        epubUndoTask?.cancel()
        epubUndo = nil
        let index = min(epubUndoIndex, epub.docData.annotations.count)
        epub.docData.annotations.insert(annotation, at: index)
        if let anchor = annotation.anchor {
            epub.webView.evaluateJavaScript(
                "applyEpubAnnotation(\(EpubTab.jsString(annotation.id)), \(anchor.chapter), \(anchor.start), \(anchor.end), \(EpubTab.jsString(annotation.kind)), \(EpubTab.jsString(annotation.color)));",
                completionHandler: nil)
        }
        epub.queueAnnotationSave()
    }

    func undoDeleteAnnotation() {
        guard let tab = pdf, let annotation = pendingUndo else { return }
        undoTask?.cancel()
        pendingUndo = nil
        let index = min(pendingUndoIndex, tab.docData.annotations.count)
        tab.docData.annotations.insert(annotation, at: index)
        renderAnnotation(annotation, in: tab)
        queueAnnotationSave(tab)
    }

    func queueAnnotationSave(_ tab: PDFTab) {
        annotationRevision &+= 1
        let revision = annotationRevision
        annotationPersistence = .pending
        tab.annoTask?.cancel()
        tab.annoTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 800_000_000)
            guard !Task.isCancelled else { return }
            await self?.storeAnnotationData(tab, revision: revision)
        }
    }

    func flushAnnotationSave(_ tab: PDFTab) {
        tab.annoTask?.cancel()
        let revision = annotationRevision
        Task { await storeAnnotationData(tab, revision: revision) }
    }

    private func storeAnnotationData(_ tab: PDFTab, revision: UInt64) async {
        guard let app, let api = app.api else {
            if revision == annotationRevision {
                annotationPersistence = .failed(L10n.t("ui.backend-not-ready"))
            }
            return
        }
        let data: Data
        do {
            data = try JSONEncoder().encode(tab.docData)
        } catch {
            if revision == annotationRevision {
                annotationPersistence = .failed(String(describing: error))
            }
            return
        }
        do {
            try await api.set_annotations(
                pdf_path: tab.path, sidecar: app.settings.annotation_sidecar, data: data)
            if revision == annotationRevision { annotationPersistence = .saved }
        } catch {
            if revision == annotationRevision {
                annotationPersistence = .failed(String(describing: error))
            }
        }
    }

    func queueEpubAnnotationSave(_ tab: EpubTab) {
        annotationRevision &+= 1
        let revision = annotationRevision
        annotationPersistence = .pending
        tab.annoTask?.cancel()
        tab.annoTask = Task { [weak self, weak tab] in
            try? await Task.sleep(nanoseconds: 800_000_000)
            guard !Task.isCancelled, let self, let tab else { return }
            await self.storeEpubAnnotationData(tab, revision: revision)
        }
    }

    func storeEpubAnnotationData(_ tab: EpubTab, revision: UInt64? = nil) async {
        let expected = revision ?? annotationRevision
        guard let app, let api = app.api else {
            if expected == annotationRevision {
                annotationPersistence = .failed(L10n.t("ui.backend-not-ready"))
            }
            return
        }
        let data: Data
        do {
            data = try JSONEncoder().encode(tab.docData)
        } catch {
            if expected == annotationRevision {
                annotationPersistence = .failed(String(describing: error))
            }
            return
        }
        do {
            try await api.set_annotations(
                pdf_path: tab.path, sidecar: app.settings.annotation_sidecar, data: data)
            if expected == annotationRevision { annotationPersistence = .saved }
        } catch {
            if expected == annotationRevision {
                annotationPersistence = .failed(String(describing: error))
            }
        }
    }

    func annotationLoadFinished(recovered: Bool) {
        annotationPersistence = recovered ? .recovered : .saved
    }

    func retryAnnotationSave() {
        annotationPersistence = .pending
        if epubActive, let epub {
            Task { await storeEpubAnnotationData(epub) }
        } else if let pdf {
            let revision = annotationRevision
            Task { await storeAnnotationData(pdf, revision: revision) }
        }
    }

    func toggleBookmark() {
        guard let tab = pdf else { return }
        if let index = tab.docData.bookmarks.firstIndex(where: { $0.page == tab.currentPage }) {
            tab.docData.bookmarks.remove(at: index)
        } else {
            tab.docData.bookmarks.append(StoredBookmark(
                page: tab.currentPage,
                label: L10n.t("ui.notes.page", "\(tab.currentPage)"),
                created: Int(Date().timeIntervalSince1970)))
        }
        tab.docData.bookmarks.sort { $0.page == $1.page ? $0.created < $1.created : $0.page < $1.page }
        tab.refreshBookmarkFlag()
        queueAnnotationSave(tab)
    }

    func removeBookmark(_ page: Int) {
        guard let tab = pdf else { return }
        tab.docData.bookmarks.removeAll { $0.page == page }
        tab.refreshBookmarkFlag()
        queueAnnotationSave(tab)
    }

    func rebuildOutline(_ tab: PDFTab) {
        var nodes: [OutlineNode] = []
        if let root = tab.document?.outlineRoot {
            for index in 0..<root.numberOfChildren {
                if let child = root.child(at: index) { nodes.append(outlineNode(child)) }
            }
        }
        tab.outline = nodes
    }

    private func outlineNode(_ outline: PDFOutline) -> OutlineNode {
        var kids: [OutlineNode] = []
        for index in 0..<outline.numberOfChildren {
            if let child = outline.child(at: index) { kids.append(outlineNode(child)) }
        }
        return OutlineNode(label: outline.label ?? "", destination: outline.destination, children: kids)
    }

    func goToOutline(_ node: OutlineNode) {
        guard let tab = pdf, let destination = node.destination else { return }
        tab.pdfView.go(to: destination)
    }

    // MARK: password prompt

    private func requestPassword(wrong: Bool) async -> String? {
        await withCheckedContinuation { continuation in
            passwordPrompt = PasswordPrompt(wrong: wrong, continuation: continuation)
        }
    }

    func submitPassword(_ password: String) {
        guard let prompt = passwordPrompt else { return }
        passwordPrompt = nil
        prompt.continuation.resume(returning: password)
    }

    func cancelPassword() {
        guard let prompt = passwordPrompt else { return }
        passwordPrompt = nil
        prompt.continuation.resume(returning: nil)
    }

    // MARK: text sources (parity with the v1 sidebar TextSource)

    func pageText(_ tab: PDFTab, page: Int) -> String? {
        guard let doc = tab.document, page >= 1, page <= doc.pageCount,
              let pageObj = doc.page(at: page - 1) else { return nil }
        return pageObj.string
    }

    func selectionText() -> String? {
        if epubActive, let epub = epub {
            let text = epub.pendingSelection?.text
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return text.count > 1 ? text : nil
        }
        guard let tab = pdf, let selection = tab.pdfView.currentSelection else { return nil }
        let text = selection.string?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return text.count > 1 ? text : nil
    }

    /// Whole-document context: first 12 pages, 3000 chars each, `--- 第 N 页 ---`
    /// markers — same payload the v1 viewer assembled.
    func docText(_ tab: PDFTab) -> (pages: Int, text: String)? {
        if epubActive, let epub = epub {
            var parts: [String] = []
            var used = 0
            for i in 1...max(1, epub.chapters) {
                let raw = (epub.textCache[i] ?? "")
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                if raw.isEmpty { continue }
                let slice = raw.count > 3000 ? String(raw.prefix(3000)) + "…" : raw
                parts.append("--- 第 \(i) 章 ---\n\(slice)")
                used += slice.count
                if used >= 24_000 { break }
            }
            return (epub.chapters, parts.joined(separator: "\n\n"))
        }
        guard let doc = tab.document, doc.pageCount > 0 else { return nil }
        let n = min(doc.pageCount, 12)
        var parts: [String] = []
        var used = 0
        for index in 0..<n {
            guard let page = doc.page(at: index),
                  let raw = page.string?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !raw.isEmpty else { continue }
            let slice = raw.count > 3000 ? String(raw.prefix(3000)) + "…" : raw
            parts.append("--- 第 \(index + 1) 页 ---\n\(slice)")
            used += slice.count
            if used >= 24_000 { break }
        }
        return (n, parts.joined(separator: "\n\n"))
    }

    func currentPageText() -> (page: Int, text: String)? {
        if epubActive, let epub = epub {
            let chapter = epub.currentPage
            var text = (epub.textCache[chapter] ?? "")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if text.isEmpty { return nil }
            if text.count > 8000 {
                text = String(text.prefix(8000)) + L10n.t("backend.text.truncated")
            }
            return (chapter, text)
        }
        guard let tab = pdf else { return nil }
        let page = tab.currentPage
        guard var text = pageText(tab, page: page)?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty else { return nil }
        if text.count > 8000 {
            text = String(text.prefix(8000)) + L10n.t("backend.text.truncated")
        }
        return (page, text)
    }

    /// Chapter text from the prefetch cache (openEpub fills it; AI, TTS and
    /// search only ever read).
    func epubTextSync(_ epub: EpubTab, chapter: Int? = nil) -> String {
        let ch = chapter ?? epub.currentPage
        return epub.textCache[ch] ?? ""
    }

    /// Prefetch every chapter's text SERIALLY so AI actions, TTS and
    /// whole-book search read from the cache without waiting. Concurrent
    /// epub RPCs stall on the embedded channel, so this must stay serial.
    func prefetchEpubText(_ epub: EpubTab) {
        guard let api = app?.api else { return }
        Task { [weak self] in
            for i in 1...max(1, epub.chapters) {
                if Task.isCancelled { return }
                let text = (try? await api.epub_chapter_text(path: epub.path, index: Int64(i))) ?? ""
                await MainActor.run { epub.textCache[i] = text }
            }
            await MainActor.run { epub.textReady = true }
        }
    }

    // MARK: AI streams

    /// v1 ensureProviderConfigured: nothing configured means the settings
    /// window opens with a notice instead of a doomed request.
    private func ensureProvider(
        for keyPath: ReferenceWritableKeyPath<ReaderModel, [StreamCard]>
    ) -> Bool {
        if app?.providerConfigured() == true { return true }
        aiTab = .translate
        app?.openSettings()
        self[keyPath: keyPath].insert(
            StreamCard.makeNotice(L10n.t("ui.settings.need-config")), at: 0)
        return false
    }

    private func prependNotice(_ message: String, to keyPath: ReferenceWritableKeyPath<ReaderModel, [StreamCard]>) {
        self[keyPath: keyPath].insert(StreamCard.makeNotice(message), at: 0)
    }

    /// Ask the shared backend to count the exact user-content strings, then
    /// present an AppKit-native, keyboard-cancellable confirmation before any
    /// provider RPC starts.  `onContinue` owns the already-captured payload,
    /// so changing pages or selections while the sheet is open cannot swap in
    /// another document's content.
    private func confirmAISend(
        action: RivetTypes.AISendAction,
        scope: RivetTypes.AISendScope,
        content: String,
        history: [[String]] = [],
        message: String = "",
        onContinue: @escaping @MainActor () -> Void
    ) {
        guard let api = app?.api else {
            alert(L10n.t("ui.backend-not-ready"))
            return
        }
        Task { [weak self] in
            do {
                let preview = try await api.preview_ai_send(
                    action: action, scope: scope, content_text: content,
                    history: history, user_message: message)
                guard let self else { return }
                let actionName = L10n.t("ui.ai-preview.action.\(preview.action.rawValue)")
                let scopeName = L10n.t("ui.ai-preview.scope.\(preview.scope.rawValue)")
                let body = L10n.t(
                    "ui.ai-preview.body", actionName, scopeName,
                    "\(preview.content_chars)", "\(preview.history_chars)",
                    "\(preview.message_chars)", "\(preview.total_chars)")
                let panel = NSAlert()
                panel.alertStyle = .informational
                panel.messageText = L10n.t("ui.ai-preview.title")
                panel.informativeText = body
                panel.addButton(withTitle: L10n.t("ui.ai-preview.continue"))
                panel.addButton(withTitle: L10n.t("ui.ai-preview.cancel"))
                let handle: (NSApplication.ModalResponse) -> Void = { response in
                    if response == .alertFirstButtonReturn { onContinue() }
                }
                if let window = NSApp.keyWindow {
                    panel.beginSheetModal(for: window, completionHandler: handle)
                } else {
                    handle(panel.runModal())
                }
            } catch {
                self?.alert(L10n.t("ui.ai-preview.failed", AppModel.cleanError(error)))
            }
        }
    }

    func removeCard(_ card: StreamCard) {
        translateCards.removeAll { $0.id == card.id }
        summarizeCards.removeAll { $0.id == card.id }
    }

    private func startCardStream(
        card: StreamCard,
        list: ReferenceWritableKeyPath<ReaderModel, [StreamCard]>,
        launch: @escaping @MainActor (RivetAPI) async throws -> RivetTypes.StreamStart
    ) {
        self[keyPath: list].insert(card, at: 0)
        guard let api = app?.api else {
            card.fail("backend not ready")
            return
        }
        Task {
            do {
                let start = try await launch(api)
                card.requestId = start.request_id
                app?.streamRegistry[start.request_id] = card
            } catch {
                card.fail(String(describing: error))
            }
        }
    }

    // translate ----------------------------------------------------------------

    func translateSelection() {
        guard let text = selectionText() else {
            prependNotice(L10n.t("ui.need.selection"), to: \.translateCards)
            return
        }
        translateSelectionText(text)
    }

    func translateSelectionText(_ text: String) {
        guard let app else { return }
        guard ensureProvider(for: \.translateCards) else { return }
        let lang = languageName(app.settings.target_language)
        let target = app.settings.target_language
        confirmAISend(action: .translate, scope: .selection, content: text) { [weak self] in
            self?.startCardStream(
                card: StreamCard(
                    title: L10n.t("ui.menu.translate"), meta: "→ \(lang)",
                    sourceText: text),
                list: \.translateCards) { api in
                try await api.translate_text(text: text, target: target)
            }
        }
    }

    func translateCurrentPage() {
        guard let app else { return }
        guard ensureProvider(for: \.translateCards) else { return }
        guard let source = currentPageText() else {
            prependNotice(L10n.t("ui.need.document"), to: \.translateCards)
            return
        }
        let lang = languageName(app.settings.target_language)
        let payload = "以下是 PDF 第 \(source.page) 页提取的文本：\n\n\(source.text)"
        let scope: RivetTypes.AISendScope = epubActive ? .chapter : .page
        let target = app.settings.target_language
        let title = epubActive ? L10n.t("ui.epub.translate-title", "\(source.page)")
                               : L10n.t("ui.translate.page-title", "\(source.page)")
        confirmAISend(action: .translate, scope: scope, content: payload) { [weak self] in
            self?.startCardStream(
                card: StreamCard(title: title, meta: "→ \(lang)", sourceText: source.text),
                list: \.translateCards) { api in
                try await api.translate_text(text: payload, target: target)
            }
        }
    }

    // summarize ----------------------------------------------------------------

    private func summarize(title: String, meta: String, text: String,
                           mode: RivetTypes.SummarizeMode,
                           scope: RivetTypes.AISendScope,
                           list: ReferenceWritableKeyPath<ReaderModel, [StreamCard]>) {
        guard let app else { return }
        guard ensureProvider(for: list) else { return }
        let lang = languageName(app.settings.target_language)
        confirmAISend(action: .summarize, scope: scope, content: text) { [weak self] in
            self?.startCardStream(
                card: StreamCard(title: title, meta: meta.isEmpty ? lang : "\(lang) · \(meta)", sourceText: text),
                list: list) { api in
                try await api.summarize_text(text: text, mode: mode)
            }
        }
    }

    func summarizePage() {
        guard let source = currentPageText() else {
            prependNotice(L10n.t("ui.need.document"), to: \.summarizeCards)
            return
        }
        summarize(
            title: epubActive ? L10n.t("ui.epub.summarize-title", "\(source.page)")
                                  : L10n.t("ui.summarize.page-title", "\(source.page)"), meta: "",
            text: source.text, mode: .page,
            scope: epubActive ? .chapter : .page, list: \.summarizeCards)
    }

    func summarizeSelection() {
        guard let text = selectionText() else {
            prependNotice(L10n.t("ui.need.selection"), to: \.summarizeCards)
            return
        }
        summarize(title: L10n.t("ui.summarize.selection"), meta: "", text: text,
                  mode: .selection, scope: .selection, list: \.summarizeCards)
    }

    func summarizeDoc() {
        guard let tab = pdf, let source = docText(tab), !source.text.isEmpty else {
            prependNotice(L10n.t("ui.need.document"), to: \.summarizeCards)
            return
        }
        summarize(
            title: L10n.t("ui.summarize.doc"),
            meta: L10n.t("ui.summarize.doc-meta", "\(source.pages)"),
            text: source.text, mode: .doc, scope: .document, list: \.summarizeCards)
    }

    // chat ---------------------------------------------------------------------

    private func chatContext() -> (text: String, label: String)? {
        switch chatScope {
        case 1:
            guard let text = selectionText() else { return nil }
            return (text, "选区")
        case 2:
            guard let tab = pdf, let source = docText(tab), !source.text.isEmpty else { return nil }
            return (source.text, "全文前 \(source.pages) 页")
        default:
            guard let page = currentPageText() else { return nil }
            return ("第 \(page.page) 页：\n\(page.text)", "第 \(page.page) 页")
        }
    }

    private func appendAssistantText(_ text: String, dimmed: Bool = false) {
        chatBubbles.append(ChatBubble(isUser: false, text: text, dimmed: dimmed, card: nil))
    }

    func sendChat(_ rawQuestion: String) {
        guard let app else { return }
        let question = rawQuestion.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !question.isEmpty, !chatBusy else { return }
        guard let context = chatContext() else {
            appendAssistantText(chatScope == 1
                ? L10n.t("ui.chat.need-selection") : L10n.t("ui.need.document"))
            return
        }
        if app.providerConfigured() {
            // ok
        } else {
            aiTab = .chat
            app.openSettings()
            appendAssistantText(L10n.t("ui.settings.need-config"))
            return
        }
        let history = Array(chatHistory.suffix(12))
        let previewScope: RivetTypes.AISendScope = switch chatScope {
        case 1: .selection
        case 2: .document
        default: epubActive ? .chapter : .page
        }
        confirmAISend(
            action: .chat, scope: previewScope, content: context.text,
            history: history, message: question
        ) { [weak self] in
            guard let self, let api = self.app?.api else { return }
            self.chatBubbles.append(ChatBubble.user(question))
            let card = StreamCard(title: "chat", meta: languageName(app.settings.target_language))
            self.chatBubbles.append(ChatBubble(isUser: false, text: "", dimmed: false, card: card))
            self.chatLive = card
            self.chatPendingQuestion = question
            self.chatBusy = true
            card.onFinish = { [weak self] finished in
                MainActor.assumeIsolated { self?.finalizeChat(finished) }
            }
            Task { [weak self] in
                do {
                    let start = try await api.chat(
                        context_text: context.text, context_label: context.label,
                        history: history, user_message: question)
                    card.requestId = start.request_id
                    self?.app?.streamRegistry[start.request_id] = card
                } catch {
                    card.fail(String(describing: error))
                    self?.finalizeChat(card)
                }
            }
        }
    }

    private func finalizeChat(_ card: StreamCard) {
        chatBusy = false
        chatLive = nil
        let hasContent = !card.content.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        if hasContent {
            chatHistory.append(["user", chatPendingQuestion])
            chatHistory.append(["assistant", card.content])
            // bubble keeps the finished card (markdown + dimmed reasoning)
            return
        }
        if !card.reasoning.isEmpty {
            // reasoning-only stream: keep the dimmed reasoning rendering
            return
        }
        guard let index = chatBubbles.lastIndex(where: { $0.card === card }) else { return }
        var bubble = chatBubbles[index]
        if case .empty(let stopped) = card.phase, stopped {
            bubble.text = L10n.t("backend.stream.stopped")
        } else {
            bubble.text = L10n.t("backend.stream.no-content")
        }
        bubble.card = nil
        bubble.dimmed = true
        chatBubbles[index] = bubble
    }

    func stopStream(_ card: StreamCard) {
        guard card.requestId > 0 else {
            card.finish(stopped: true)
            if chatLive === card { finalizeChat(card) }
            return
        }
        stoppingStreams.insert(card.requestId)
        Task { [weak self] in try? await self?.app?.api?.stop_stream(request_id: card.requestId) }
    }

    func stopChat() {
        if let card = chatLive { stopStream(card) }
    }

    // MARK: find

    func runFind() {
        findTask?.cancel()
        let query = findQuery
        if epubActive, let epub = epub {
            runEpubFind(query, epub)
            return
        }
        guard let tab = pdf, !query.isEmpty else {
            findTotal = 0
            findIndex = 0
            clearFindHighlights()
            return
        }
        findTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 250_000_000)
            guard !Task.isCancelled else { return }
            await MainActor.run { self?.performFind(query: query, tab: tab) }
        }
    }

    private func performFind(query: String, tab: PDFTab) {
        guard let doc = tab.document else { return }
        let hits = doc.findString(query, withOptions: [.caseInsensitive])
        findHits = hits
        findTotal = hits.count
        tab.pdfView.highlightedSelections = hits.isEmpty ? nil : hits
        guard !hits.isEmpty else { return }
        let current = max(tab.currentPage - 1, 0)
        let pagesIndex = { (selection: PDFSelection) -> Int in
            guard let page = selection.pages.first else { return 0 }
            return doc.index(for: page)
        }
        findIndex = hits.firstIndex(where: { pagesIndex($0) >= current }) ?? 0
        tab.pdfView.go(to: hits[findIndex])
    }

    func findNext() { navigateFind(1) }

    func findPrevious() { navigateFind(-1) }

    /// Whole-book search over the prefetched chapter texts (v1 runSearch):
    /// case-insensitive, per-chapter ordinals, excerpt-free navigation.
    private func runEpubFind(_ query: String, _ epub: EpubTab) {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        epubFindHits = []
        epubFindQuery = needle
        guard !needle.isEmpty else {
            findTotal = 0
            findIndex = 0
            epub.clearHighlights()
            return
        }
        for i in 1...max(1, epub.chapters) {
            let text = (epub.textCache[i] ?? "").lowercased()
            var from = text.startIndex
            var ordinal = 0
            while let range = text.range(of: needle.lowercased(), range: from..<text.endIndex) {
                epubFindHits.append((i, ordinal))
                ordinal += 1
                from = range.upperBound
            }
        }
        findTotal = epubFindHits.count
        findIndex = epubFindHits.isEmpty ? 0 : 0
        if let first = epubFindHits.first {
            epub.highlightHit(chapter: first.chapter, ordinal: first.ordinal, query: needle)
        }
    }

    private func navigateFind(_ step: Int) {
        if epubActive, let epub = epub {
            guard !epubFindHits.isEmpty else { return }
            epubFindHitIndex = (epubFindHitIndex + step + epubFindHits.count) % epubFindHits.count
            let hit = epubFindHits[epubFindHitIndex]
            findIndex = epubFindHitIndex
            epub.highlightHit(chapter: hit.chapter, ordinal: hit.ordinal, query: epubFindQuery)
            return
        }
        guard let tab = pdf, !findHits.isEmpty else { return }
        findIndex = (findIndex + step + findHits.count) % findHits.count
        tab.pdfView.go(to: findHits[findIndex])
    }

    func closeFind() {
        findVisible = false
        findQuery = ""
        findTotal = 0
        epubFindHits = []
        epubFindHitIndex = 0
        epub?.clearHighlights()
        clearFindHighlights()
    }

    private func clearFindHighlights() {
        findHits = []
        pdf?.pdfView.highlightedSelections = nil
    }
}
