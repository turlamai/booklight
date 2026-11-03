import SwiftUI
import AppKit
import PDFKit
import UniformTypeIdentifiers
import RivetEmbedding
import RivetRuntime

/// App-wide model: the embedded Racket backend, settings/presets/recents and
/// the updater. One per process; every reader window shares it. Per-window
/// document state lives in ReaderModel.
@MainActor
final class AppModel: ObservableObject {
    @Published var ready = false
    @Published var status = ""
    @Published var settings = RivetTypes.SettingsView(
        provider: "deepseek", base_url: "", model: "", target_language: .zh,
        view_mode: "single", annotation_sidecar: true,
        reading_theme: .system, reading_font_scale_permille: 1000,
        has_api_key: false, recent: [])
    @Published var presets: [RivetTypes.ProviderPreset] = []
    @Published var recents: [RivetTypes.RecentEntry] = []

    // updater state (backend verifies/downloads; this model polls)
    @Published var updateState: UpdateState = .idle
    @Published var showUpdateSheet = false
    /// metadata of the newest available update (version, size); shown in the sheet
    @Published var updateInfo: UpdateCheckResult?
    private var updatePollTimer: Timer?
    private var updatePollFailures = 0

    /// The reader window that owns menu actions (the key window), registered
    /// by WindowAccessor when its window becomes key.
    weak var activeReader: ReaderModel?

    /// Live reader windows — display-mode settings apply across all of them.
    private var liveReaders: [Weak<ReaderModel>] = []
    private var closingAnnotationFlushes: [UUID: Task<Void, Never>] = [:]

    /// Stream routing: RVT1 events carry only a request id; the registry
    /// points at the card a reader window created for it. Cards are owned by
    /// the reader's published arrays; this map only delivers events.
    var streamRegistry: [Int64: StreamCard] = [:]
    private var stoppingStreams: Set<Int64> = []

    /// Present a new reader window (nil = welcome state) or open a file.
    /// Installed by the first reader window from its openWindow environment
    /// (the App/Commands layer has no environment access for this).
    var openWindow: ((String?) -> Void)?

    /// In-app language override: "system" | "zh" | "en". Applied at launch
    /// (UI strings are baked per render, so switching needs a relaunch —
    /// the Settings pane offers the button).
    @Published var languageSetting: String

    /// File to open in the first reader window at launch (PDFGIST_OPEN,
    /// standing in for file association in dev builds — the bare SwiftPM
    /// binary creates no window for a positional argument).
    private var pendingLaunch: String?

    private var backend: EmbeddedRacketBackend?
    private(set) var api: RivetAPI?
    private var bootstrapTask: Task<Void, Never>?

    /// Document loads await settings (especially annotation storage mode)
    /// before choosing central vs sidecar. Otherwise opening via Finder on
    /// launch can read the wrong location and hide a recovery draft.
    func waitForBootstrap() async {
        if ready { return }
        await bootstrapTask?.value
    }

    init() {
        // v1 UI is Chinese-first; en.json exists, so follow the system locale
        // unless the in-app override says otherwise (Locale.preferredLanguages
        // also honors the per-app language override in System Settings).
        // Identity migration: the language preference may still live in the
        // pre-booklight defaults domain.
        let stored = UserDefaults.standard.string(forKey: "booklight-lang")
            ?? (CFPreferencesCopyAppValue("pdfgist-lang" as CFString,
                                          "site.jrtx.pdfgist" as CFString) as? String)
            ?? "system"
        languageSetting = stored
        L10n.language = Self.resolvedLanguage(stored)
        if let launch = ProcessInfo.processInfo.environment["BOOKLIGHT_OPEN"]
            ?? ProcessInfo.processInfo.environment["PDFGIST_OPEN"],
           ["pdf", "epub"].contains(URL(fileURLWithPath: launch).pathExtension.lowercased()) {
            pendingLaunch = launch
        }
    }

    static func resolvedLanguage(_ setting: String) -> String {
        switch setting {
        case "zh": return "zh"
        case "en": return "en"
        default: return Locale.preferredLanguages.first?.hasPrefix("zh") == false ? "en" : "zh"
        }
    }

    func setLanguage(_ setting: String) {
        languageSetting = setting
        UserDefaults.standard.set(setting, forKey: "booklight-lang")
    }

    /// Relaunch into the newly chosen language: open first, terminate only
    /// once the new instance is confirmed coming up.
    func relaunchNow() {
        guard let bundleURL = Bundle.main.bundleURL as URL? else { return }
        NSWorkspace.shared.openApplication(at: bundleURL, configuration: NSWorkspace.OpenConfiguration()) { _, _ in
            Task { @MainActor in
                NSApp.terminate(nil)
            }
        }
    }

    func claimPendingLaunch() -> String? {
        defer { pendingLaunch = nil }
        return pendingLaunch
    }

    // MARK: reader registry

    func register(_ reader: ReaderModel) {
        liveReaders.append(Weak(reader))
        liveReaders.removeAll { $0.value == nil }
    }

    func unregister(_ reader: ReaderModel) {
        liveReaders.removeAll { $0.value === reader }
        if activeReader === reader { activeReader = nil }
        // Keep the reader alive until its close-time annotation handoff has
        // reached the backend. The task is retained so application
        // termination can await it instead of racing a closed window.
        let id = UUID()
        let task = Task { @MainActor [weak self, reader] in
            await reader.flushAnnotationsBeforeTermination()
            self?.closingAnnotationFlushes.removeValue(forKey: id)
        }
        closingAnnotationFlushes[id] = task
    }

    func firstPendingPDFEditReader() -> ReaderModel? {
        liveReaders.compactMap { $0.value }.first { $0.pdfEditsPending }
    }

    func flushAnnotationsBeforeTermination() async {
        let readers = liveReaders.compactMap { $0.value }
        for reader in readers {
            await reader.flushAnnotationsBeforeTermination()
        }
        let closing = Array(closingAnnotationFlushes.values)
        for task in closing { await task.value }
    }

    func applyDisplayModeToAll() {
        let double = settings.view_mode == "double"
        for reader in liveReaders.compactMap({ $0.value }) {
            reader.pdf?.setDisplayMode(double: double)
        }
    }

    /// The reader page surface ("light"/"dark") for the stored reading
    /// theme: paper/night force it, system follows the effective app
    /// appearance (which the reader windows already track).
    func readingSurface() -> String {
        switch settings.reading_theme {
        case .paper: return "light"
        case .night: return "dark"
        case .sepia: return "sepia"
        case .system: return EpubTab.systemTheme()
        }
    }

    /// Push the resolved page surface into every open reader window.
    func applyReadingThemeToAll() {
        let surface = readingSurface()
        for reader in liveReaders.compactMap({ $0.value }) {
            reader.applyReadingSurface(surface)
        }
    }

    /// The settings-pane reading-theme picker: persists through the narrow
    /// reading RPC and re-skins open books immediately.
    func saveReadingPrefs(theme: RivetTypes.ReadingTheme) async -> String {
        guard let api else { return "backend not ready" }
        do {
            settings = try await api.save_reading_prefs(
                theme: theme,
                font_scale_permille: settings.reading_font_scale_permille)
            applyReadingThemeToAll()
            return L10n.t("ui.settings.saved")
        } catch {
            return L10n.t("ui.settings.save-failed", String(describing: error))
        }
    }

    /// EPUB typography steps (A+/A-) persist best-effort per click; a
    /// failure only means the scale resets next launch.
    func saveReadingFontScale(_ scale: Double) {
        guard let api else { return }
        let permille = Int64((scale * 1000).rounded())
        Task { [weak self] in
            guard let self, let api = self.api else { return }
            do {
                self.settings = try await api.save_reading_prefs(
                    theme: self.settings.reading_theme,
                    font_scale_permille: permille)
            } catch { /* best-effort */ }
        }
    }

    // MARK: backend lifecycle

    func start() {
        guard backend == nil else { return }
        installHostActions()
        UpdaterInstaller.cleanupStaleBackup()

        do {
            let config = try EmbeddedRacketConfiguration.resolvedDefault(
                moduleName: RivetGeneratedConfig.moduleName,
                entryName: RivetGeneratedConfig.entryName)
            let backend = EmbeddedRacketBackend(configuration: config)
            let relay = EventRelay(app: self)
            self.backend = backend
            status = "Starting embedded Racket CS…"
            // taskly-family start pattern: the embedded runtime handshake is
            // quick, so start synchronously on the main actor and keep every
            // use of self inside MainActor-isolated code — no detached-task
            // captures for older Swift 6 toolchains (the Intel release
            // runner's Xcode 16.4) to reject as data races.
            try backend.start { [weak relay] name, value in
                guard let event = try? RivetEvent.decode(name: name, value: value) else { return }
                Task { @MainActor in relay?.receive(event) }
            }
            // Racket CS takes over SIGTERM at boot and never restores the
            // default disposition, so `kill` bounces off the running app
            // (the 0.3.0 stuck-update incident). Hooks go in AFTER the
            // runtime boots, not before (rivet syncpilot#3 precedent).
            signal(SIGTERM, SIG_DFL)
            let api = RivetAPI(client: backend.client)
            self.api = api
            bootstrapTask = Task { await bootstrap() }
        } catch {
            status = "Configuration error: \(error)"
        }
    }

    /// Post-start initialization, all on the main actor: locale, the initial
    /// settings/presets/recents fetches.
    private func bootstrap() async {
        guard let api else { return }
        do {
            try await api.initialize()
            // Backend error strings follow the host locale.
            try? await api.set_locale(code: L10n.language)
            async let fetchedSettings = api.get_settings()
            async let fetchedPresets = api.list_presets()
            async let fetchedRecents = api.get_recents()
            let (s, p, r) = try await (fetchedSettings, fetchedPresets, fetchedRecents)
            settings = s
            presets = p
            recents = r
            ready = true
            status = ""
            // File-first: settings and a small recent-file list are enough.
            // Do not catalog opened files or start a reading-time tracker.
            autoCheckForUpdates()
        } catch {
            ready = false
            status = String(describing: error)
        }
    }

    // MARK: events

    fileprivate func receive(_ event: RivetEvent) {
        switch event {
        case .stream_chunk(let chunk):
            streamRegistry[chunk.request_id]?.push(chunk.delta, reasoning: chunk.is_reasoning)
        case .stream_done(let done):
            finishStream(requestId: done.request_id, error: nil)
        case .stream_error(let error):
            finishStream(requestId: error.request_id, error: error.message)
        }
    }

    private func finishStream(requestId: Int64, error: String?) {
        guard let card = streamRegistry.removeValue(forKey: requestId) else { return }
        let stopped = stoppingStreams.remove(requestId) != nil
        if let error {
            card.fail(error)
        } else {
            card.finish(stopped: stopped)
        }
        // The owning reader finalizes chat state through this hook.
        card.onFinish?(card)
    }

    // MARK: streams (shared plumbing used by ReaderModel)

    func stopStream(_ card: StreamCard) {
        guard card.requestId > 0 else {
            card.finish(stopped: true)
            card.onFinish?(card)
            return
        }
        stoppingStreams.insert(card.requestId)
        Task { [weak self] in try? await self?.api?.stop_stream(request_id: card.requestId) }
    }

    /// Stop every live stream owned by a closing window so the backend does
    /// not keep generating into nowhere.
    func stopStreams(_ requests: [Int64]) {
        for requestId in requests {
            stoppingStreams.insert(requestId)
            streamRegistry.removeValue(forKey: requestId)
            Task { [weak self] in try? await self?.api?.stop_stream(request_id: requestId) }
        }
    }

    // MARK: recents

    func refreshRecents() async {
        if let recents = try? await api?.get_recents() {
            self.recents = recents
        }
    }

    func noteRecent(path: String, page: Int, of total: Int) async {
        guard ready else { return }
        let pages = max(total, 1)
        // Page-based scroll estimate; resume only uses the page number.
        let ratio = Int64((Double(page) / Double(pages) * 100_000).rounded())
        _ = try? await api?.update_recents(
            path: path, page: Int64(page), scroll_ratio_scaled: ratio)
        await refreshRecents()
        // Mirror into the system recent-documents list (Dock recents).
        NSDocumentController.shared.noteNewRecentDocumentURL(URL(fileURLWithPath: path))
    }

    func recentPage(_ path: String) -> Int {
        recents.first { $0.path == path }.map { Int($0.page) } ?? 1
    }

    // MARK: settings (Settings scene)

    func saveApiKey(_ key: String) async -> String {
        guard let api else { return "backend not ready" }
        do {
            try await api.save_api_key(key: key)
            settings = try await api.get_settings()
            return L10n.t("ui.settings.saved")
        } catch {
            return L10n.t("ui.settings.save-failed", String(describing: error))
        }
    }

    func testConnection(baseURL: String, apiKey: String, model: String) async -> (message: String, models: [String]) {
        guard let api else { return ("backend not ready", []) }
        do {
            let result = try await api.test_connection(base_url: baseURL, api_key: apiKey, model: model)
            if result.ok {
                let models = result.models ?? []
                let message = models.isEmpty
                    ? L10n.t("ui.settings.test-ok-no-list")
                    : L10n.t("ui.settings.test-ok", "\(models.count)")
                return (message, models)
            }
            return (result.error ?? "unknown error", [])
        } catch {
            return (String(describing: error), [])
        }
    }

    func saveSettings(
        provider: String, baseURL: String, model: String,
        target: RivetTypes.TargetLanguage, viewMode: String, sidecar: Bool
    ) async -> String {
        guard let api else { return "backend not ready" }
        let update = RivetTypes.SettingsUpdate(
            provider: provider, base_url: baseURL, model: model,
            target_language: target, view_mode: viewMode, annotation_sidecar: sidecar)
        do {
            settings = try await api.save_settings(update: update)
            applyDisplayModeToAll()
            return L10n.t("ui.settings.saved")
        } catch {
            return L10n.t("ui.settings.save-failed", String(describing: error))
        }
    }

    /// v1 ensureProviderConfigured: no base URL/model/key (except local
    /// Ollama) means the AI action is not available yet.
    func providerConfigured() -> Bool {
        !settings.base_url.isEmpty && !settings.model.isEmpty
            && (settings.has_api_key || settings.provider == "ollama")
    }

    func openSettings() {
        // The SwiftUI Settings scene registers this selector on macOS 14.
        NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
    }

    // MARK: updates (payback family pattern: check → download → install)

    /// Silent launch-time check, throttled server-side to once a day;
    /// only an available update opens the sheet (a failure never nags).
    private func autoCheckForUpdates() {
        Task {
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            checkUpdates(throttled: true, present: false)
        }
    }

    func checkForUpdates() {
        checkUpdates(throttled: false, present: true)
    }

    private func checkUpdates(throttled: Bool, present: Bool) {
        guard let api else {
            if present {
                updateState = .error(L10n.t("ui.backend-not-ready"))
                showUpdateSheet = true
            }
            return
        }
        Task {
            do {
                let data = try await api.check_updates(force: !throttled)
                let result = try JSONDecoder().decode(UpdateCheckResult.self, from: data)
                if result.status == "available" && present {
                    updateInfo = result
                    updateState = .idle
                    showUpdateSheet = true
                } else if result.status == "up-to-date" && present {
                    updateState = .upToDate
                    showUpdateSheet = true
                } else if result.status == "error" && present {
                    updateState = .error(result.message ?? "unknown error")
                    showUpdateSheet = true
                }
            } catch {
                if present {
                    updateState = .error("\(error)")
                    showUpdateSheet = true
                }
            }
        }
    }

    func startDownload() {
        guard let api else { return }
        // optimistic: the first state poll (0.4s) confirms with real progress
        updateState = UpdateState(phase: "downloading", percent: 0, message: nil,
                                  downloadedPath: nil,
                                  availableVersion: updateInfo?.availableVersion)
        Task {
            do {
                _ = try await api.start_download()
                startPolling()
            } catch {
                updateState = .error(Self.cleanError(error))
            }
        }
    }

    private func startPolling() {
        updatePollTimer?.invalidate()
        updatePollFailures = 0
        let timer = Timer(timeInterval: 0.4, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollUpdateState() }
        }
        RunLoop.main.add(timer, forMode: .common)
        updatePollTimer = timer
        pollUpdateState()
    }

    private func pollUpdateState() {
        guard let api else { return }
        Task { [weak self] in
            do {
                let data = try await api.update_state()
                let state = try JSONDecoder().decode(UpdateState.self, from: data)
                await MainActor.run {
                    guard let self else { return }
                    self.updatePollFailures = 0
                    self.updateState = state
                    if state.phase != "downloading" {
                        self.updatePollTimer?.invalidate()
                        self.updatePollTimer = nil
                    }
                }
            } catch {
                // 0.2.x stopped the timer on the first transient RPC error,
                // freezing the progress bar forever (the stuck-update
                // incident). Retry through hiccups; only give up — loudly —
                // after a sustained silence.
                await MainActor.run {
                    guard let self else { return }
                    self.updatePollFailures += 1
                    if self.updatePollFailures >= 8 {
                        self.updatePollTimer?.invalidate()
                        self.updatePollTimer = nil
                        self.updateState = .error(
                            L10n.t("ui.update.install-failed", AppModel.cleanError(error)))
                    }
                }
            }
        }
    }

    func installUpdate() {
        guard updateState.phase == "downloaded",
              let path = updateState.downloadedPath, !path.isEmpty else {
            // Never a silent no-op: reaching here with the wrong state means
            // the sheet drifted from the model — say so and offer retry.
            updateState = .error(L10n.t("ui.update.install-failed", L10n.t("ui.update.state-stale")))
            return
        }
        updateState = UpdateState(
            phase: "installing", percent: 100, message: nil,
            downloadedPath: path, availableVersion: updateState.availableVersion)
        Task { [weak self] in
            do {
                try UpdaterInstaller.install(artifactAt: URL(fileURLWithPath: path))
                let launched = await UpdaterInstaller.relaunchInstalled()
                guard !launched else { return } // old process ended via terminate
                await MainActor.run {
                    // The swap worked but the new bundle would not launch:
                    // the running (old) app stays alive and reports it.
                    self?.updateState = .error(
                        L10n.t("ui.update.install-failed", L10n.t("ui.update.relaunch-failed")))
                }
            } catch {
                await MainActor.run {
                    self?.updateState = .error(
                        L10n.t("ui.update.install-failed", AppModel.cleanError(error)))
                }
            }
        }
    }

    /// RVT1 failures arrive as "...error: <message>"; keep the message.
    static func cleanError(_ error: Error) -> String {
        let text = "\(error)"
        if let range = text.range(of: "error: ") {
            return String(text[range.upperBound...])
        }
        return text
    }

    // MARK: menu action routing

    private func installHostActions() {
        HostActions.open = { [weak self] in self?.pickAndOpen() }
        HostActions.find = { [weak self] in self?.activeReader?.openFind() }
        HostActions.printDocument = { [weak self] in self?.activeReader?.printDocument() }
        HostActions.toggleBookmark = { [weak self] in self?.activeReader?.toggleBookmark() }
        HostActions.toggleTTS = { [weak self] in self?.activeReader?.toggleTTS() }
        HostActions.toggleSplit = { [weak self] in self?.activeReader?.toggleSplit() }
        HostActions.zoomIn = { [weak self] in self?.activeReader?.pdf?.zoomIn() }
        HostActions.zoomOut = { [weak self] in self?.activeReader?.pdf?.zoomOut() }
        HostActions.zoomReset = { [weak self] in self?.activeReader?.pdf?.zoomReset() }
        HostActions.fitWidth = { [weak self] in self?.activeReader?.pdf?.fitWidth() }
        HostActions.setDisplayMode = { [weak self] double in
            guard let pdf = self?.activeReader?.pdf else { return }
            pdf.setDisplayMode(double: double)
            // Reading preference: persist through the settings round trip
            // (SettingsView fields are immutable snapshots).
            let s = self?.settings
            Task {
                _ = await self?.saveSettings(
                    provider: s?.provider ?? "deepseek", baseURL: s?.base_url ?? "",
                    model: s?.model ?? "", target: s?.target_language ?? .zh,
                    viewMode: double ? "double" : "single",
                    sidecar: s?.annotation_sidecar ?? false)
            }
        }
        HostActions.page = { [weak self] step in
            self?.activeReader?.goPage(step)
        }
        HostActions.firstPage = { [weak self] in self?.activeReader?.goToEdgePage(last: false) }
        HostActions.lastPage = { [weak self] in self?.activeReader?.goToEdgePage(last: true) }
        HostActions.pageEdit = { [weak self] op in self?.activeReader?.runPageEdit(op) }
        HostActions.mergePDF = { [weak self] in self?.activeReader?.pickAndMergePDF() }
        HostActions.showForms = { [weak self] in self?.activeReader?.showForms() }
        HostActions.showWatermark = { [weak self] in self?.activeReader?.showWatermarkDialog() }
        HostActions.exportPageImage = { [weak self] in self?.activeReader?.exportCurrentPageImage() }
        HostActions.exportText = { [weak self] in self?.activeReader?.exportDocumentText() }
        HostActions.exportPDF = { [weak self] in self?.activeReader?.exportEditedPdf() }
    }

    /// Open panel shared by the menu and the welcome state. Empty reader
    /// windows take the first file; everything else opens in a new window.
    func pickAndOpen() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.pdf, .epub]
        panel.message = L10n.t("ui.menu.open")
        let handle: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK else { return }
            let urls = panel.urls
            guard !urls.isEmpty, let self else { return }
            // Open the selected files at their original paths. Use the
            // active empty window for the first; every other file gets a
            // document window. There is no separate catalog import.
            let paths = urls.map { $0.path }
            if let reader = self.activeReader, !reader.hasDocument {
                Task { await reader.openPath(paths[0]) }
            } else {
                self.openWindow?(paths[0])
            }
            for path in paths.dropFirst() {
                self.openWindow?(path)
            }
        }
        if let window = NSApp.keyWindow {
            panel.beginSheetModal(for: window, completionHandler: handle)
        } else {
            handle(panel.runModal())
        }
    }
}

struct Weak<T: AnyObject> {
    weak var value: T?
    init(_ value: T?) { self.value = value }
}

// MARK: - action hooks for the Commands layer

/// App-wide action hooks so the Commands menus can reach the key window's
/// reader without threading the model through the scene graph.
enum HostActions {
    nonisolated(unsafe) static var open: (() -> Void)?
    nonisolated(unsafe) static var find: (() -> Void)?
    nonisolated(unsafe) static var toggleBookmark: (() -> Void)?
    nonisolated(unsafe) static var printDocument: (() -> Void)?
    nonisolated(unsafe) static var toggleTTS: (() -> Void)?
    nonisolated(unsafe) static var toggleSplit: (() -> Void)?
    nonisolated(unsafe) static var zoomIn: (() -> Void)?
    nonisolated(unsafe) static var zoomOut: (() -> Void)?
    nonisolated(unsafe) static var zoomReset: (() -> Void)?
    nonisolated(unsafe) static var fitWidth: (() -> Void)?
    nonisolated(unsafe) static var setDisplayMode: ((Bool) -> Void)?
    nonisolated(unsafe) static var page: ((Int) -> Void)?
    nonisolated(unsafe) static var firstPage: (() -> Void)?
    nonisolated(unsafe) static var lastPage: (() -> Void)?
    nonisolated(unsafe) static var pageEdit: ((PageEditOp) -> Void)?
    nonisolated(unsafe) static var mergePDF: (() -> Void)?
    nonisolated(unsafe) static var showForms: (() -> Void)?
    nonisolated(unsafe) static var showWatermark: (() -> Void)?
    nonisolated(unsafe) static var exportPageImage: (() -> Void)?
    nonisolated(unsafe) static var exportText: (() -> Void)?
    nonisolated(unsafe) static var exportPDF: (() -> Void)?
}

/// Fields of the watermark sheet; applied through edit-bake-text.
struct WatermarkDraft: Equatable {
    var text: String = "机密"
    var size: Int = 48
    var opacityPercent: Int = 30
}

/// RVT1 event bridge: events arrive on a background hop and are delivered to
/// the app model on the main actor.
@MainActor
private final class EventRelay {
    private weak var app: AppModel?
    init(app: AppModel) { self.app = app }
    func receive(_ event: RivetEvent) { app?.receive(event) }
    func fail(_ message: String) {
        guard let app else { return }
        app.ready = false
        app.status = message
    }
}
