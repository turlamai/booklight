import SwiftUI
import AppKit
import PDFKit

// MARK: - research workbench (design/prototype/research.html)
//
// Cross-book search over open and recent documents plus a traceable
// citation deck. Free feature, no gating (PRICING.md 2026-11-18 owner
// decision). Search executes host-side over the text layers each host
// already owns — the EPUB chapter cache, PDFKit page strings for open
// PDFs, and on-demand PDFKit/RPC extraction for not-open recents. Pinned
// citations are `kind: "cite"` entries in the same per-document
// annotation store, so save, recovery and export come for free.

@MainActor
final class ResearchModel: ObservableObject {
    struct Target: Identifiable, Hashable {
        let path: String
        let title: String
        let kind: String  // "pdf" | "epub"
        let isOpen: Bool
        var id: String { path }
    }

    struct Hit: Identifiable, Hashable {
        let target: Target
        let page: Int          // 1-based page, or chapter ordinal for EPUBs
        let excerpt: String
        let matchStart: Int    // within excerpt
        let matchLen: Int
        var id: String { "\(target.path)#\(page)#\(matchStart)" }
    }

    struct Cite: Identifiable {
        let path: String
        let title: String
        let page: Int
        let excerpt: String
        let note: String
        let annoId: String
        let kind: String       // "pdf" | "epub"
        let isOpen: Bool
        var id: String { "\(path)#\(annoId)" }
    }

    @Published var query = ""
    @Published var targets: [Target] = []
    @Published var selected: Set<String> = []
    @Published var results: [Hit] = []
    @Published var searching = false
    @Published var searched = false
    @Published var elapsedSeconds = 0.0
    @Published var deck: [Cite] = []
    @Published var deckLoading = false

    private var searchTask: Task<Void, Never>?
    private var refreshTask: Task<Void, Never>?
    weak var app: AppModel?

    private static let perDocumentCap = 20
    private static let totalCap = 50
    private static let excerptBefore = 40
    private static let excerptAfter = 80
    private static let recentsDeckLimit = 8

    func attach(_ app: AppModel) {
        if self.app == nil { self.app = app }
        refreshTargets()
        refreshDeck()
    }

    // MARK: scope

    func refreshTargets() {
        guard let app else { return }
        var opened: [Target] = []
        for reader in app.openReaders where reader.hasDocument {
            let kind = reader.epubActive ? "epub" : "pdf"
            let path = reader.documentPath ?? ""
            let title = reader.documentTitle
                ?? (path as NSString).lastPathComponent
            opened.append(Target(path: path, title: title,
                                 kind: kind, isOpen: true))
        }
        var recents: [Target] = []
        let openedPaths = Set(opened.map(\.path))
        for entry in app.recents where !openedPaths.contains(entry.path) {
            recents.append(Target(
                path: entry.path,
                title: (entry.path as NSString).lastPathComponent,
                kind: (entry.path as NSString).pathExtension.lowercased() == "epub"
                    ? "epub" : "pdf",
                isOpen: false))
        }
        targets = opened + recents
        // Open documents are always in scope; recents join on click.
        selected.formUnion(Set(opened.map(\.path)))
        selected = selected.intersection(Set(targets.map(\.path)))
    }

    func toggleScope(_ target: Target) {
        guard !target.isOpen else { return }
        if selected.contains(target.path) {
            selected.remove(target.path)
        } else {
            selected.insert(target.path)
        }
    }

    // MARK: search

    func runSearch() {
        searchTask?.cancel()
        guard let app else { return }
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else {
            results = []
            searched = false
            searching = false
            return
        }
        let scope = targets.filter { selected.contains($0.path) }
        guard !scope.isEmpty else {
            results = []
            searched = true
            searching = false
            return
        }
        searching = true
        searched = false
        results = []
        let startedAt = CFAbsoluteTimeGetCurrent()
        searchTask = Task { [weak self] in
            var hits: [Hit] = []
            for target in scope {
                if Task.isCancelled { return }
                guard let self else { return }
                let units = await self.textUnits(for: target, app: app)
                hits.append(contentsOf: Self.matches(
                    in: units, target: target, needle: needle))
                if hits.count >= Self.totalCap { break }
            }
            let elapsed = CFAbsoluteTimeGetCurrent() - startedAt
            await MainActor.run { [weak self] in
                guard let self, !Task.isCancelled else { return }
                self.results = Array(hits.prefix(Self.totalCap))
                self.elapsedSeconds = (elapsed * 10).rounded() / 10
                self.searching = false
                self.searched = true
            }
        }
    }

    /// (page-or-chapter, text) units for one target. Open documents read
    /// their existing host-side text layers; not-open recents are
    /// extracted on demand (PDFKit for PDFs, the chapter-text RPC for
    /// EPUBs) without opening a reader window.
    private func textUnits(for target: Target, app: AppModel) async -> [(page: Int, text: String)] {
        if let reader = app.reader(atPath: target.path) {
            if reader.epubActive, let epub = reader.epub {
                return epub.textCache
                    .sorted { $0.key < $1.key }
                    .map { (page: $0.key, text: $0.value) }
            }
            if let tab = reader.pdf {
                var units: [(page: Int, text: String)] = []
                for page in 1...max(1, tab.pageCount) {
                    if let text = reader.pageText(tab, page: page), !text.isEmpty {
                        units.append((page: page, text: text))
                    }
                }
                return units
            }
            return []
        }
        // Not open: extract on demand.
        if target.kind == "epub" {
            guard let api = app.api else { return [] }
            do {
                let view = try await api.epub_open(path: target.path)
                var units: [(page: Int, text: String)] = []
                let count = max(1, Int(view.chapters))
                for chapter in 1...count {
                    if Task.isCancelled { return [] }
                    let text = (try? await api.epub_chapter_text(
                        path: target.path, index: Int64(chapter))) ?? ""
                    if !text.isEmpty { units.append((page: chapter, text: text)) }
                }
                return units
            } catch {
                return []
            }
        }
        guard let doc = PDFDocument(url: URL(fileURLWithPath: target.path)) else { return [] }
        var units: [(page: Int, text: String)] = []
        for index in 0..<doc.pageCount {
            if Task.isCancelled { return [] }
            if let text = doc.page(at: index)?.string, !text.isEmpty {
                units.append((page: index + 1, text: text))
            }
        }
        return units
    }

    private static func matches(
        in units: [(page: Int, text: String)], target: Target, needle: String
    ) -> [Hit] {
        var hits: [Hit] = []
        let foldedNeedle = needle.lowercased()
        for unit in units {
            let folded = unit.text.lowercased()
            var cursor = folded.startIndex
            while let range = folded.range(of: foldedNeedle, range: cursor..<folded.endIndex) {
                // Window the excerpt in index space, not byte space, so
                // CJK text keeps clean boundaries.
                let startIdx = unit.text.index(
                    range.lowerBound,
                    offsetBy: -excerptBefore,
                    limitedBy: unit.text.startIndex) ?? unit.text.startIndex
                let endIdx = unit.text.index(
                    range.upperBound,
                    offsetBy: excerptAfter,
                    limitedBy: unit.text.endIndex) ?? unit.text.endIndex
                let excerpt = String(unit.text[startIdx..<endIdx])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                let matchStart = excerpt.lowercased().range(of: foldedNeedle)?.lowerBound
                let start = matchStart.map { excerpt.distance(from: excerpt.startIndex, to: $0) } ?? 0
                hits.append(Hit(
                    target: target, page: unit.page, excerpt: excerpt,
                    matchStart: start, matchLen: needle.count))
                if hits.count >= perDocumentCap { return hits }
                cursor = range.upperBound
                if cursor >= folded.endIndex { break }
            }
        }
        return hits
    }

    // MARK: citation deck

    func isPinned(_ hit: Hit) -> Bool {
        deck.contains {
            $0.path == hit.target.path && $0.page == hit.page
                && $0.excerpt == hit.excerpt
        }
    }

    func togglePin(_ hit: Hit) {
        if let existing = deck.first(where: {
            $0.path == hit.target.path && $0.page == hit.page
                && $0.excerpt == hit.excerpt
        }) {
            unpin(existing)
        } else {
            pin(hit)
        }
    }

    private func pin(_ hit: Hit) {
        guard let app else { return }
        let reader = app.reader(atPath: hit.target.path)
        if let reader {
            _ = reader.addCitation(page: hit.page, excerpt: hit.excerpt)
        } else {
            // Not open: rewrite that document's store through the RPC.
            Task { [weak self] in
                guard let self, let app = self.app, let api = app.api else { return }
                let sidecar = app.settings.annotation_sidecar
                guard let data = try? await api.get_annotations(
                    pdf_path: hit.target.path, sidecar: sidecar),
                    var doc = try? JSONDecoder().decode(DocData.self, from: data)
                else { return }
                let annotation = StoredAnnotation(
                    id: String(format: "c-%lld-%04d",
                               Int(Date().timeIntervalSince1970),
                               Int.random(in: 0..<10_000)),
                    page: hit.page, rects: [], excerpt: hit.excerpt,
                    color: "", kind: "cite", note: "",
                    created: Int(Date().timeIntervalSince1970))
                doc.annotations.append(annotation)
                guard let encoded = try? JSONEncoder().encode(doc),
                      let bytes = try? await api.set_annotations(
                        pdf_path: hit.target.path, sidecar: sidecar, data: encoded)
                else { return }
                _ = bytes
                await MainActor.run { self.refreshDeck() }
            }
        }
        refreshDeck()
    }

    private func unpin(_ cite: Cite) {
        guard let app else { return }
        if let reader = app.reader(atPath: cite.path) {
            reader.removeCitation(id: cite.annoId)
        } else {
            Task { [weak self] in
                guard let self, let app = self.app, let api = app.api else { return }
                let sidecar = app.settings.annotation_sidecar
                guard let data = try? await api.get_annotations(
                    pdf_path: cite.path, sidecar: sidecar),
                    var doc = try? JSONDecoder().decode(DocData.self, from: data)
                else { return }
                doc.annotations.removeAll { $0.id == cite.annoId }
                guard let encoded = try? JSONEncoder().encode(doc)
                else { return }
                _ = try? await api.set_annotations(
                    pdf_path: cite.path, sidecar: sidecar, data: encoded)
                await MainActor.run { self.refreshDeck() }
            }
        }
        refreshDeck()
    }

    func refreshDeck() {
        guard let app else { return }
        refreshTask?.cancel()
        refreshTask = Task { [weak self] in
            guard let self else { return }
            var cites: [Cite] = []
            for reader in app.openReaders where reader.hasDocument {
                let kind = reader.epubActive ? "epub" : "pdf"
                let path = reader.documentPath ?? ""
                let title = reader.documentTitle
                    ?? (path as NSString).lastPathComponent
                for annotation in reader.citeAnnotations {
                    cites.append(Cite(
                        path: path, title: title,
                        page: annotation.page, excerpt: annotation.excerpt,
                        note: annotation.note, annoId: annotation.id,
                        kind: kind, isOpen: true))
                }
            }
            let openPaths = Set(cites.map(\.path))
            let recents = app.recents.filter { !openPaths.contains($0.path) }
                .prefix(Self.recentsDeckLimit)
            for entry in recents {
                if Task.isCancelled { return }
                guard let api = app.api else { break }
                let sidecar = app.settings.annotation_sidecar
                guard let data = try? await api.get_annotations(
                    pdf_path: entry.path, sidecar: sidecar),
                    let doc = try? JSONDecoder().decode(DocData.self, from: data)
                else { continue }
                let title = (entry.path as NSString).lastPathComponent
                let kind = (entry.path as NSString).pathExtension.lowercased() == "epub"
                    ? "epub" : "pdf"
                for annotation in doc.annotations where annotation.kind == "cite" {
                    cites.append(Cite(
                        path: entry.path, title: title, page: annotation.page,
                        excerpt: annotation.excerpt, note: annotation.note,
                        annoId: annotation.id, kind: kind, isOpen: false))
                }
            }
            await MainActor.run { [weak self] in
                guard let self, !Task.isCancelled else { return }
                self.deck = cites
            }
        }
    }

    // MARK: actions

    func jump(to hit: Hit) {
        guard let app else { return }
        if let reader = app.reader(atPath: hit.target.path) {
            if reader.epubActive {
                reader.epub?.goToChapter(hit.page)
            } else {
                reader.pdf?.goToPage(hit.page)
            }
        }
        // Open/focus the document window (opening preserves file-first: no
        // import, just the original path).
        app.openWindow?(hit.target.path)
    }

    func copyCitation(forExcerpt excerpt: String, title: String, page: Int, kind: String) {
        let location = kind == "epub"
            ? L10n.t("ui.epub.chapter-n", "\(page)")
            : L10n.t("ui.notes.page", "\(page)")
        let citation = L10n.t("ui.research.cite-format", title, location, excerpt)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(citation, forType: .string)
    }

    func copyCitation(for hit: Hit) {
        copyCitation(forExcerpt: hit.excerpt, title: hit.target.title,
                     page: hit.page, kind: hit.target.kind)
    }

    func copyCitation(for cite: Cite) {
        copyCitation(forExcerpt: cite.excerpt, title: cite.title,
                     page: cite.page, kind: cite.kind)
    }

    func jumpBack(to cite: Cite) {
        guard let app else { return }
        if let reader = app.reader(atPath: cite.path) {
            if reader.epubActive {
                reader.epub?.goToChapter(cite.page)
            } else {
                reader.pdf?.goToPage(cite.page)
            }
        }
        app.openWindow?(cite.path)
    }
}

// MARK: - window

struct ResearchWindowView: View {
    @EnvironmentObject private var app: AppModel
    @StateObject private var model = ResearchModel()

    var body: some View {
        ResearchView(model: model)
            .environmentObject(app)
            .onAppear { model.attach(app) }
            .navigationTitle(L10n.t("ui.research.title"))
            .navigationSubtitle(L10n.t("ui.research.subtitle"))
    }
}

// MARK: - view (design/prototype/research.html layout)

struct ResearchView: View {
    @ObservedObject var model: ResearchModel
    @EnvironmentObject private var app: AppModel

    var body: some View {
        HStack(spacing: 0) {
            scopeColumn
                .frame(width: 232)
            Divider().overlay(Theme.border)
            mainColumn
            if !model.deck.isEmpty || model.deckLoading {
                Divider().overlay(Theme.border)
                deckColumn
                    .frame(width: 288)
            }
        }
        .frame(minWidth: 980, minHeight: 600)
        .background(Theme.sidebar)
    }

    // MARK: scope sidebar

    private var scopeColumn: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(L10n.t("ui.research.scope"))
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Theme.textDim)
                .padding(.horizontal, 16)
                .padding(.top, 14)
                .padding(.bottom, 6)

            let opened = model.targets.filter(\.isOpen)
            if !opened.isEmpty {
                Text(L10n.t("ui.research.opened", "\(opened.count)"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.text)
                    .padding(.horizontal, 16)
                    .padding(.bottom, 4)
                ForEach(opened) { target in
                    scopeRow(target, checked: true, interactive: false)
                }
            }

            let recents = model.targets.filter { !$0.isOpen }
            if !recents.isEmpty {
                Text(L10n.t("ui.research.recent-docs"))
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(Theme.textDim)
                    .padding(.horizontal, 16)
                    .padding(.top, 12)
                    .padding(.bottom, 4)
                ForEach(recents) { target in
                    scopeRow(target, checked: model.selected.contains(target.path),
                             interactive: true)
                }
            }

            Spacer()
            Text(L10n.t("ui.research.footer"))
                .font(.system(size: 11))
                .foregroundStyle(Theme.textFaint)
                .lineLimit(3)
                .padding(.horizontal, 16)
                .padding(.bottom, 14)
        }
    }

    private func scopeRow(_ target: ResearchModel.Target, checked: Bool,
                          interactive: Bool) -> some View {
        Button {
            if interactive { model.toggleScope(target) }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: checked ? "checkmark" : "plus")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(checked ? Theme.accent : Theme.textFaint)
                    .frame(width: 14)
                Text(target.title)
                    .font(.system(size: 12.5))
                    .foregroundStyle(interactive && !checked
                                     ? Theme.textDim : Theme.text)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 6)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(interactive ? target.path : "")
    }

    // MARK: main column

    private var mainColumn: some View {
        VStack(spacing: 0) {
            queryHead
                .padding(.horizontal, 20)
                .padding(.top, 14)
                .padding(.bottom, 10)
            if model.searching {
                banner
                    .padding(.horizontal, 20)
                    .padding(.bottom, 8)
            }
            if !model.deck.isEmpty {
                deckStatusBar
            }
            resultsList
            statusBar
        }
        .background(Theme.bg)
    }

    private var queryHead: some View {
        HStack(spacing: 10) {
            HStack(spacing: 9) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 13))
                    .foregroundStyle(Theme.textDim)
                TextField(L10n.t("ui.research.query-placeholder"), text: $model.query)
                    .textFieldStyle(.plain)
                    .font(.system(size: 14))
                    .onSubmit { model.runSearch() }
                    .accessibilityIdentifier("booklight.research.query")
            }
            .padding(.horizontal, 14)
            .frame(height: 36)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                    .fill(Theme.panel))
            .overlay(
                RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                    .stroke(Theme.border, lineWidth: 1))

            Button(L10n.t("ui.research.search")) {
                model.runSearch()
            }
            .buttonStyle(.borderedProminent)
            .tint(Theme.accent)
            .controlSize(.large)
            .disabled(model.query.trimmingCharacters(in: .whitespaces).isEmpty
                      || model.searching)
            .keyboardShortcut(.defaultAction)
            .accessibilityIdentifier("booklight.research.search")
        }
    }

    private var banner: some View {
        HStack(spacing: 8) {
            ProgressView()
                .controlSize(.small)
            Text(L10n.t("ui.research.searching",
                        "\(model.targets.filter { model.selected.contains($0.path) }.count)"))
                .font(.system(size: 12, weight: .medium))
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(Theme.accentSoft))
    }

    /// The deck's status line rides the main column while the deck itself
    /// hides on an empty deck (prototype keeps the footer visible).
    private var deckStatusBar: some View { EmptyView() }

    @ViewBuilder
    private var resultsList: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 8) {
                if model.searched && model.results.isEmpty && !model.searching {
                    if model.targets.isEmpty {
                        emptyDocs
                    } else {
                        noResults
                    }
                }
                ForEach(groupedResults, id: \.target.id) { group in
                    Text(L10n.t("ui.research.hits", group.target.title,
                                "\(group.hits.count)"))
                        .font(.system(size: 12, weight: .bold))
                        .foregroundStyle(Theme.textDim)
                        .padding(.top, 10)
                    ForEach(group.hits) { hit in
                        resultCard(hit)
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 16)
        }
    }

    private var groupedResults: [(target: ResearchModel.Target, hits: [ResearchModel.Hit])] {
        var order: [ResearchModel.Target] = []
        var buckets: [String: [ResearchModel.Hit]] = [:]
        for hit in model.results {
            if buckets[hit.target.path] == nil { order.append(hit.target) }
            buckets[hit.target.path, default: []].append(hit)
        }
        return order.map { ($0, buckets[$0.path] ?? []) }
    }

    private var emptyDocs: some View {
        VStack(spacing: 8) {
            Text(L10n.t("ui.research.no-docs"))
                .font(.system(size: 14, weight: .semibold))
            Text(L10n.t("ui.research.no-docs-hint"))
                .font(.system(size: 12.5))
                .foregroundStyle(Theme.textDim)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 60)
    }

    private var noResults: some View {
        VStack(spacing: 8) {
            Text(L10n.t("ui.research.no-result-title"))
                .font(.system(size: 14, weight: .semibold))
            Text(L10n.t("ui.research.no-result-hint"))
                .font(.system(size: 12.5))
                .foregroundStyle(Theme.textDim)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 60)
    }

    private func resultCard(_ hit: ResearchModel.Hit) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(hit.excerpt.attributedHighlight(
                range: hit.matchStart..<(hit.matchStart + hit.matchLen),
                color: Theme.accentSoft))
                .font(.system(size: 13.5))
                .lineSpacing(3)
                .textSelection(.enabled)
            HStack(spacing: 6) {
                sourceChip(hit.target, page: hit.page) {
                    model.jump(to: hit)
                }
                Spacer(minLength: 0)
                cardButton(L10n.t("ui.research.jump")) { model.jump(to: hit) }
                cardButton(L10n.t("ui.research.cite")) { model.copyCitation(for: hit) }
                cardButton(
                    model.isPinned(hit)
                        ? L10n.t("ui.research.pinned")
                        : L10n.t("ui.research.pin"),
                    emphasized: model.isPinned(hit)) {
                        model.togglePin(hit)
                    }
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(Theme.panel))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .stroke(Theme.border, lineWidth: 1))
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text(verbatim: hit.excerpt))
    }

    private func sourceChip(_ target: ResearchModel.Target, page: Int,
                            action: @escaping () -> Void) -> some View {
        let location = target.kind == "epub"
            ? L10n.t("ui.epub.chapter-n", "\(page)")
            : L10n.t("ui.notes.page", "\(page)")
        return Button {
            action()
        } label: {
            HStack(spacing: 3) {
                Text("\(target.title) · \(location)")
                    .font(.system(size: 11, weight: .semibold))
                    .lineLimit(1)
                Image(systemName: "arrow.up.right")
                    .font(.system(size: 8, weight: .semibold))
            }
            .foregroundStyle(Theme.accent)
            .padding(.horizontal, 9)
            .padding(.vertical, 2)
            .background(Capsule().fill(Theme.accentSoft))
        }
        .buttonStyle(.plain)
    }

    private func cardButton(_ title: String, emphasized: Bool = false,
                            action: @escaping () -> Void) -> some View {
        Button {
            action()
        } label: {
            Text(title)
                .font(.system(size: 11, weight: emphasized ? .semibold : .regular))
                .padding(.horizontal, 9)
                .padding(.vertical, 3)
        }
        .buttonStyle(.plain)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(emphasized ? Theme.accentSoft : Color.clear))
        .foregroundStyle(emphasized ? Theme.accent : Theme.textDim)
    }

    private var statusBar: some View {
        HStack(spacing: 14) {
            Text(L10n.t("ui.research.footer"))
                .font(.system(size: 11.5))
                .lineLimit(1)
            Spacer(minLength: 0)
            if model.searched {
                Text(L10n.t("ui.research.stats",
                            "\(model.targets.filter { model.selected.contains($0.path) }.count)",
                            "\(model.results.count)",
                            String(format: "%.1f", model.elapsedSeconds)))
                    .font(.system(size: 11.5))
                    .foregroundStyle(Theme.textDim)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 7)
        .background(Theme.sidebar)
        .overlay(alignment: .top) { Divider().overlay(Theme.border) }
    }

    // MARK: citation deck

    private var deckColumn: some View {
        VStack(spacing: 0) {
            HStack {
                Text(L10n.t("ui.research.deck"))
                    .font(.system(size: 13, weight: .bold))
                Spacer(minLength: 0)
                Text(L10n.t("ui.research.deck-count", "\(model.deck.count)"))
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(Theme.accent)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 1)
                    .background(Capsule().fill(Theme.accentSoft))
            }
            .padding(.horizontal, 14)
            .padding(.top, 12)
            .padding(.bottom, 4)
            Text(L10n.t("ui.research.deck-footer"))
                .font(.system(size: 11))
                .foregroundStyle(Theme.textFaint)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 14)
                .padding(.bottom, 8)
            ScrollView {
                LazyVStack(spacing: 8) {
                    ForEach(model.deck) { cite in
                        citeCard(cite)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.bottom, 12)
            }
        }
        .background(Theme.sidebar)
    }

    private func citeCard(_ cite: ResearchModel.Cite) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text("「\(cite.excerpt)」")
                .font(.system(size: 12.5))
                .lineSpacing(3)
                .textSelection(.enabled)
            HStack(spacing: 5) {
                let location = cite.kind == "epub"
                    ? L10n.t("ui.epub.chapter-n", "\(cite.page)")
                    : L10n.t("ui.notes.page", "\(cite.page)")
                Text("《\(cite.title)》· \(location)")
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(1)
                Spacer(minLength: 0)
                Button {
                    model.jumpBack(to: cite)
                } label: {
                    Text(L10n.t("ui.research.jump-back"))
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Theme.accent)
                }
                .buttonStyle(.plain)
            }
            if !cite.note.isEmpty {
                Text(cite.note)
                    .font(.system(size: 11.5))
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(3)
                    .padding(.top, 2)
            }
            HStack {
                cardButton(L10n.t("ui.research.cite")) { model.copyCitation(for: cite) }
                cardButton(L10n.t("ui.notes.delete-annotation"), emphasized: false) {
                    model.unpinFromDeck(cite)
                }
                Spacer(minLength: 0)
            }
        }
        .padding(11)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(Theme.panel))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .stroke(Theme.border, lineWidth: 1))
    }
}

extension ResearchModel {
    /// Deck-facing unpin (the card's remove action routes here so the
    /// refreshes stay in one place).
    func unpinFromDeck(_ cite: Cite) {
        unpin(cite)
    }
}

extension String {
    /// 命中高亮（prototype research.html <mark>）：accent-soft 背景落在
    /// 摘句的命中区间上。
    func attributedHighlight(range: Range<Int>, color: Color) -> AttributedString {
        var attributed = AttributedString(self)
        let lower = max(0, min(range.lowerBound, count))
        let upper = max(lower, min(range.upperBound, count))
        guard lower < upper else { return attributed }
        let start = attributed.index(attributed.startIndex, offsetByCharacters: lower)
        let end = attributed.index(attributed.startIndex, offsetByCharacters: upper)
        attributed[start..<end].backgroundColor = color
        return attributed
    }
}
