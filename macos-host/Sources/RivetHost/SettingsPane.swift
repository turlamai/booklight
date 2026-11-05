import SwiftUI
import AppKit

/// App-wide preferences, using the platform's grouped form rather than a
/// hand-built column: native labels, focus rings, section spacing and
/// keyboard traversal come for free and match System Settings.
struct SettingsPane: View {
    @EnvironmentObject private var app: AppModel

    @State private var providerID = "deepseek"
    @State private var baseURL = ""
    @State private var modelField = ""
    @State private var apiKey = ""
    @State private var target: RivetTypes.TargetLanguage = .zh
    @State private var viewMode = "single"
    @State private var sidecar = false
    @State private var status = ""
    @State private var statusIsError = false
    @State private var modelSuggestions: [String] = []
    @State private var busy = false
    @State private var loaded = false
    @State private var presetStash: [String: (url: String, model: String)] = [:]
    @State private var loadedLanguage = "system"

    var body: some View {
        Form {
            Section {
                Picker(L10n.t("ui.settings.preset"), selection: Binding(
                    get: { providerID },
                    set: { switchPreset($0) })) {
                    ForEach(app.presets, id: \.id) { preset in
                        Text(preset.label).tag(preset.id)
                    }
                }

                TextField(L10n.t("ui.settings.base-url"), text: $baseURL,
                          prompt: Text(verbatim: "https://…"))
                    .textContentType(.URL)
                    .autocorrectionDisabled()

                TextField(L10n.t("ui.settings.model"), text: $modelField,
                          prompt: Text(verbatim: "deepseek-chat"))

                if !modelSuggestions.isEmpty {
                    Picker(L10n.t("ui.settings.suggested-models"), selection: Binding(
                        get: { modelField },
                        set: { modelField = $0 })) {
                        ForEach(modelSuggestions, id: \.self) { suggestion in
                            Text(suggestion).tag(suggestion)
                        }
                    }
                }

                SecureField(
                    L10n.t("ui.settings.api-key"),
                    text: $apiKey,
                    prompt: Text(app.settings.has_api_key
                                 ? L10n.t("ui.settings.api-key-saved") : "sk-…"))

                HStack(spacing: 8) {
                    Button(L10n.t("ui.settings.save-key")) {
                        Task { await saveKey() }
                    }
                    .disabled(apiKey.isEmpty || busy)

                    Button(L10n.t("ui.settings.test")) {
                        Task { await testConnection() }
                    }
                    .disabled(busy || !canTest)

                    Button(L10n.t("ui.settings.fetch-models")) {
                        Task { await testConnection() }
                    }
                    .disabled(busy || !canTest)

                    if busy {
                        ProgressView().controlSize(.small)
                    }
                    Spacer()
                }
                .buttonStyle(.bordered)
                .controlSize(.small)

                if !status.isEmpty {
                    Text(status)
                        .font(.footnote)
                        .foregroundStyle(statusIsError ? Theme.danger : Theme.textDim)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                }
            } header: {
                Text(L10n.t("ui.settings.ai-section"))
            } footer: {
                Text(L10n.t("ui.settings.privacy"))
                    .font(.footnote)
                    .foregroundStyle(Theme.textFaint)
            }

            Section(L10n.t("ui.settings.appearance")) {
                Picker(L10n.t("ui.settings.target-language"), selection: $target) {
                    ForEach(RivetTypes.TargetLanguage.allCasesArray, id: \.self) { code in
                        Text(languageName(code)).tag(code)
                    }
                }

                Picker(L10n.t("ui.settings.view-mode"), selection: $viewMode) {
                    Text(L10n.t("ui.settings.view-single")).tag("single")
                    Text(L10n.t("ui.settings.view-double")).tag("double")
                }
                .pickerStyle(.segmented)

                Picker(L10n.t("ui.reading.theme"), selection: Binding(
                    get: { app.settings.reading_theme },
                    set: { newValue in
                        Task { _ = await app.saveReadingPrefs(theme: newValue) }
                    })) {
                    Text(L10n.t("ui.reading.theme.system")).tag(RivetTypes.ReadingTheme.system)
                    Text(L10n.t("ui.reading.theme.paper")).tag(RivetTypes.ReadingTheme.paper)
                    Text(L10n.t("ui.reading.theme.sepia")).tag(RivetTypes.ReadingTheme.sepia)
                    Text(L10n.t("ui.reading.theme.night")).tag(RivetTypes.ReadingTheme.night)
                }
                .pickerStyle(.segmented)
                .accessibilityIdentifier("settings.reading-theme")

                Toggle(L10n.t("ui.settings.sidecar"), isOn: Binding(
                    get: { sidecar },
                    set: { sidecar = $0 }))

                HStack {
                    Spacer()
                    Button(L10n.t("ui.settings.save")) {
                        Task { await save() }
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .disabled(busy)
                }
            }

            Section(L10n.t("ui.settings.language")) {
                Picker(L10n.t("ui.settings.language"), selection: Binding(
                    get: { app.languageSetting },
                    set: { app.setLanguage($0) })) {
                    Text(L10n.t("ui.settings.language-system")).tag("system")
                    Text(verbatim: "中文").tag("zh")
                    Text(verbatim: "English").tag("en")
                }
                .pickerStyle(.segmented)

                Text(L10n.t("ui.settings.language-note"))
                    .font(.footnote)
                    .foregroundStyle(Theme.textFaint)

                if app.languageSetting != loadedLanguage {
                    Button(L10n.t("ui.settings.relaunch")) {
                        app.relaunchNow()
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
            }

            Section {
                LabeledContent(L10n.t("app.name"), value: "\(RivetGeneratedConfig.version) (\(RivetGeneratedConfig.build))")
                HStack(spacing: 16) {
                    Link(L10n.t("ui.about.website"),
                         destination: URL(string: "https://booklight.jrtx.site")!)
                    Link(L10n.t("ui.about.issue"),
                         destination: URL(string: "https://github.com/turlamai/booklight/issues")!)
                }
                .font(.footnote)
                Text(L10n.t("ui.about.license"))
                    .font(.footnote)
                    .foregroundStyle(Theme.textFaint)
            }
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .background(Theme.bg)
        .onAppear {
            if !loaded {
                loadFromSettings()
                loadedLanguage = app.languageSetting
                loaded = true
            }
        }
    }

    // MARK: helpers

    private var canTest: Bool {
        !baseURL.trimmingCharacters(in: .whitespaces).isEmpty
            && (!modelField.trimmingCharacters(in: .whitespaces).isEmpty || app.settings.has_api_key)
    }

    private func loadFromSettings() {
        providerID = app.settings.provider
        baseURL = app.settings.base_url
        modelField = app.settings.model
        target = app.settings.target_language
        viewMode = app.settings.view_mode == "double" ? "double" : "single"
        sidecar = app.settings.annotation_sidecar
    }

    /// v1 preset behavior: stash the edited URL/model under the old preset,
    /// restore the stash (or the preset defaults) for the new one.
    private func switchPreset(_ newID: String) {
        presetStash[providerID] = (baseURL, modelField)
        providerID = newID
        if let stash = presetStash[newID] {
            baseURL = stash.url
            modelField = stash.model
        } else if let preset = app.presets.first(where: { $0.id == newID }) {
            baseURL = preset.base_url
            modelField = preset.default_model
        } else {
            baseURL = ""
            modelField = ""
        }
        apiKey = ""
        modelSuggestions = []
    }

    private func saveKey() async {
        busy = true
        status = L10n.t("ui.settings.saving-key")
        defer { busy = false }
        status = await app.saveApiKey(apiKey.trimmingCharacters(in: .whitespaces))
        statusIsError = !status.hasPrefix("✓")
        apiKey = ""
    }

    private func testConnection() async {
        busy = true
        status = L10n.t("ui.settings.fetching")
        statusIsError = false
        defer { busy = false }
        let result = await app.testConnection(
            baseURL: baseURL.trimmingCharacters(in: .whitespaces),
            apiKey: apiKey.trimmingCharacters(in: .whitespaces),
            model: modelField.trimmingCharacters(in: .whitespaces))
        status = result.message
        statusIsError = !result.message.hasPrefix("✓")
        modelSuggestions = result.models
    }

    private func save() async {
        busy = true
        defer { busy = false }
        status = await app.saveSettings(
            provider: providerID,
            baseURL: baseURL.trimmingCharacters(in: .whitespaces),
            model: modelField.trimmingCharacters(in: .whitespaces),
            target: target,
            viewMode: viewMode,
            sidecar: sidecar)
        statusIsError = !status.hasPrefix("✓")
    }
}

extension RivetTypes.TargetLanguage {
    static var allCasesArray: [RivetTypes.TargetLanguage] {
        [.zh, .zh_hant, .en, .ja, .ko, .fr, .de, .es]
    }
}
