import SwiftUI
import AppKit
import PDFKit

// MARK: - reader sidebar (left, pure current-document navigation)
//
// Thumbnails / outline / annotations for PDFs, chapter TOC for EPUBs. No
// recents here — those live in File > Open Recent and the welcome state.

struct ReaderSidebar: View {
    @EnvironmentObject private var app: AppModel
    @ObservedObject var reader: ReaderModel

    var body: some View {
        VStack(spacing: 0) {
            sidebarHeader
            Divider().overlay(Theme.border)
            Group {
                if reader.epubActive, let epub = reader.epub {
                    if reader.sidebarMode == 2 {
                        EpubNotesPane(epub: epub, reader: reader, onExport: {
                            reader.exportEpubAnnotations()
                        })
                    } else {
                        EpubTocList(tab: epub)
                    }
                } else if let tab = reader.pdf {
                    panelContent(tab)
                } else {
                    emptyHint
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Theme.sidebar)
    }

    /// The sidebar owns its view switcher. Keeping it out of the titlebar
    /// leaves the document title in the native centered position and makes
    /// the control's scope unambiguous.
    private var sidebarHeader: some View {
        HStack(spacing: 8) {
            if reader.epubActive, reader.epub != nil {
                Picker("", selection: Binding(
                    get: { reader.sidebarMode == 2 ? 2 : 1 },
                    set: { reader.sidebarMode = $0 })) {
                    Label(L10n.t("ui.viewer.outline"), systemImage: "list.bullet.indent")
                        .tag(1)
                        .accessibilityLabel(Text(L10n.t("ui.viewer.outline")))
                    Label(L10n.t("ui.sidebar.annotations"), systemImage: "highlighter")
                        .tag(2)
                        .accessibilityLabel(Text(L10n.t("ui.sidebar.annotations")))
                        .accessibilityIdentifier("booklight.sidebar.annotations")
                }
                .pickerStyle(.segmented)
                .labelStyle(.iconOnly)
                .labelsHidden()
                .controlSize(.small)
                .help(L10n.t("ui.toolbar.panel"))
                .accessibilityIdentifier("booklight.sidebar.picker")
            } else if reader.pdf != nil {
                Picker("", selection: $reader.sidebarMode) {
                    Label(L10n.t("ui.viewer.thumbs"), systemImage: "square.grid.2x2").tag(0)
                    Label(L10n.t("ui.viewer.outline"), systemImage: "list.bullet.indent")
                        .tag(1)
                        .accessibilityLabel(Text(L10n.t("ui.viewer.outline")))
                    Label(L10n.t("ui.sidebar.annotations"), systemImage: "highlighter")
                        .tag(2)
                        .accessibilityLabel(Text(L10n.t("ui.sidebar.annotations")))
                        .accessibilityIdentifier("booklight.sidebar.annotations")
                    Label(L10n.t("ui.sidebar.forms"), systemImage: "list.bullet.rectangle").tag(3)
                }
                .pickerStyle(.segmented)
                .labelStyle(.iconOnly)
                .labelsHidden()
                .controlSize(.small)
                .help(L10n.t("ui.toolbar.panel"))
                .accessibilityIdentifier("booklight.sidebar.picker")
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
    }

    @ViewBuilder
    private func panelContent(_ tab: PDFTab) -> some View {
        switch reader.sidebarMode {
        case 0:
            ThumbnailStrip(tab: tab)
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
        case 1:
            OutlineList(tab: tab)
        case 2:
            NotesPane(tab: tab, reader: reader, onExport: {
                reader.exportAnnotations()
            })
        default:
            FormsPane(tab: tab)
        }
    }

    private var emptyHint: some View {
        ContentUnavailableView {
            Label(L10n.t("ui.welcome.open-any"), systemImage: "doc.text")
        }
        .foregroundStyle(Theme.textFaint)
    }
}

// MARK: - outline

struct OutlineList: View {
    @EnvironmentObject private var app: AppModel
    let tab: PDFTab

    private struct FlatEntry {
        let id: UUID
        let pageIndex: Int
    }

    private func flatten(_ nodes: [OutlineNode], doc: PDFDocument?,
                         into out: inout [FlatEntry]) {
        for node in nodes {
            var pageIndex = -1
            if let page = node.destination?.page {
                pageIndex = doc?.index(for: page) ?? -1
            }
            out.append(FlatEntry(id: node.id, pageIndex: pageIndex))
            flatten(node.children, doc: doc, into: &out)
        }
    }

    private var activeID: UUID? {
        guard let doc = tab.document else { return nil }
        var entries: [FlatEntry] = []
        flatten(tab.outline, doc: doc, into: &entries)
        var best: FlatEntry?
        for entry in entries where entry.pageIndex >= 0
            && entry.pageIndex < tab.currentPage {
            if best == nil || entry.pageIndex >= best?.pageIndex ?? -1 {
                best = entry
            }
        }
        return best?.id
    }

    private var selection: Binding<UUID?> {
        Binding(
            get: { activeID },
            set: { id in
                guard let id, let node = Self.find(id, in: tab.outline) else { return }
                app.activeReader?.goToOutline(node)
            })
    }

    var body: some View {
        if tab.outline.isEmpty {
            ContentUnavailableView {
                Label(L10n.t("ui.viewer.outline-empty"), systemImage: "list.bullet.indent")
            }
        } else {
            ScrollViewReader { proxy in
                List(selection: selection) {
                    OutlineGroup(tab.outline, children: \.subitems) { node in
                        Text(node.label.isEmpty ? " " : node.label)
                            .font(.system(size: 12))
                            .lineLimit(2)
                            .tag(node.id)
                            .id(node.id)
                    }
                }
                .listStyle(.sidebar)
                .scrollContentBackground(.hidden)
                .onChange(of: tab.currentPage) { _, _ in
                    if let id = activeID {
                        withAnimation(.easeIn(duration: 0.15)) {
                            proxy.scrollTo(id, anchor: .center)
                        }
                    }
                }
                .onAppear {
                    if let id = activeID { proxy.scrollTo(id, anchor: .center) }
                }
            }
        }
    }

    private static func find(_ id: UUID, in nodes: [OutlineNode]) -> OutlineNode? {
        for node in nodes {
            if node.id == id { return node }
            if let hit = find(id, in: node.children) { return hit }
        }
        return nil
    }
}

private extension OutlineNode {
    var subitems: [OutlineNode]? {
        children.isEmpty ? nil : children
    }
}

struct EpubTocList: View {
    @ObservedObject var tab: EpubTab

    private var selection: Binding<Int?> {
        Binding(
            get: { tab.currentPage },
            set: { chapter in
                if let chapter { tab.goToChapter(chapter) }
            })
    }

    /// Some EPUBs repeat a chapter entry as its own child (or duplicate the
    /// same target twice). Collapse exact duplicates before handing the list
    /// to SwiftUI, which would otherwise render a stale twin row.
    private var tocItems: [RivetTypes.EpubTocItem] {
        var seen = Set<String>()
        return tab.toc.filter { item in
            let key = "\(item.chapter)|\(item.title)"
            guard !seen.contains(key) else { return false }
            seen.insert(key)
            return true
        }
    }

    var body: some View {
        if tab.toc.isEmpty && tab.chapters <= 1 {
            ContentUnavailableView {
                Label(L10n.t("ui.epub.toc-empty"), systemImage: "list.bullet.indent")
            }
        } else {
            ScrollViewReader { proxy in
                List(selection: selection) {
                    if tab.toc.isEmpty {
                        ForEach(1...max(1, tab.chapters), id: \.self) { chapter in
                            Text(L10n.t("ui.epub.chapter-n", "\(chapter)"))
                                .font(.system(size: 12))
                                .tag(chapter)
                                .id(chapter)
                        }
                    } else {
                        ForEach(Array(tocItems.enumerated()), id: \.offset) { _, item in
                            Text(item.title)
                                .font(.system(size: 12))
                                .lineLimit(2)
                                .padding(.leading, CGFloat(item.level) * 12)
                                .tag(Int(item.chapter))
                                .id(Int(item.chapter))
                        }
                    }
                }
                .listStyle(.sidebar)
                .scrollContentBackground(.hidden)
                .onChange(of: tab.currentPage) { _, chapter in
                    withAnimation(.easeIn(duration: 0.15)) {
                        proxy.scrollTo(chapter, anchor: .center)
                    }
                }
                .onAppear { proxy.scrollTo(tab.currentPage, anchor: .center) }
            }
        }
    }
}

// MARK: - EPUB annotations (chapter+offset anchored; marks live in the page)

struct EpubNotesPane: View {
    @ObservedObject var epub: EpubTab
    @ObservedObject var reader: ReaderModel
    let onExport: () -> Void

    private var sorted: [StoredAnnotation] {
        epub.docData.annotations.sorted {
            $0.page == $1.page ? $0.created < $1.created : $0.page < $1.page
        }
    }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 10) {
                AnnotationSaveBanner(reader: reader, onExport: onExport)
                if let selection = epub.pendingSelection, selection.text.count > 1 {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(L10n.t("ui.notes.epub-annotate"))
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(Theme.textDim)
                        HStack(spacing: 6) {
                            annotatePill(L10n.t("ui.menu.highlight-yellow"), "yellow", "highlight")
                            annotatePill(L10n.t("ui.menu.highlight-green"), "green", "highlight")
                            annotatePill(L10n.t("ui.menu.highlight-blue"), "blue", "highlight")
                            annotatePill(L10n.t("ui.menu.underline"), "", "underline")
                            annotatePill(L10n.t("ui.menu.strike"), "", "strike")
                        }
                    }
                    .padding(10)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                            .fill(Theme.panel))
                }
                if reader.epubUndo != nil {
                    HStack(spacing: 8) {
                        Text(L10n.t("ui.notes.deleted"))
                            .font(.caption)
                            .foregroundStyle(Theme.textDim)
                        Spacer()
                        Button(L10n.t("ui.undo")) {
                            reader.undoDeleteEpubAnnotation()
                        }
                        .font(.caption.weight(.semibold))
                        .buttonStyle(.plain)
                        .foregroundStyle(Theme.accent)
                    }
                    .padding(10)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                            .fill(Theme.panel))
                }
                if epub.docData.annotations.isEmpty {
                    ContentUnavailableView {
                        Label(L10n.t("ui.notes.empty"), systemImage: "highlighter")
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.top, 8)
                }
                ForEach(sorted) { annotation in
                    EpubNoteCard(annotation: annotation, reader: reader)
                }
                if !sorted.isEmpty {
                    Button(action: onExport) {
                        Label(L10n.t("ui.notes.export"), systemImage: "square.and.arrow.up")
                            .font(.caption)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.accent)
                    .padding(.top, 2)
                }
            }
            .padding(10)
        }
    }

    private func annotatePill(_ title: String, _ color: String, _ kind: String)
        -> some View {
        Button {
            reader.addEpubAnnotation(color: color, kind: kind)
            epub.pendingSelection = nil
        } label: {
            Text(title)
                .font(.system(size: 11))
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Capsule().fill(Theme.panelAlt))
                .overlay(Capsule().stroke(Theme.border, lineWidth: 0.5))
        }
        .buttonStyle(.plain)
        .foregroundStyle(Theme.text)
    }
}

struct EpubNoteCard: View {
    let annotation: StoredAnnotation
    @ObservedObject var reader: ReaderModel
    @State private var hovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 7) {
                Circle()
                    .fill(Theme.annotationSwiftUIColor(annotation.color))
                    .frame(width: 8, height: 8)
                Text(L10n.t("ui.epub.chapter-n", "\(annotation.page)"))
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(Theme.textDim)
                Spacer(minLength: 0)
                menu.opacity(hovering ? 1 : 0.45)
            }
            Text(annotation.excerpt)
                .font(.system(size: 12))
                .lineLimit(4)
                .fixedSize(horizontal: false, vertical: true)
            if !annotation.note.isEmpty {
                Text(annotation.note)
                    .font(.caption)
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(3)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(hovering ? Theme.panelAlt : Theme.panel))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .contextMenu { actions }
    }

    private var menu: some View {
        Menu {
            actions
        } label: {
            Image(systemName: "ellipsis")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Theme.textDim)
                .frame(width: 18, height: 18)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
    }

    @ViewBuilder
    private var actions: some View {
        Button(L10n.t("ui.menu.translate")) {
            reader.translateSelectionText(annotation.excerpt)
        }
        Divider()
        Button(L10n.t("ui.notes.delete-annotation"), role: .destructive) {
            reader.removeEpubAnnotation(annotation.id)
        }
    }
}

// MARK: - right AI inspector: 翻译 / 总结 / 对话 / 表单
//
// Annotations and bookmarks live in the reader sidebar; settings live in
// the Settings scene (⌘,).

struct AISidebar: View {
    @ObservedObject var reader: ReaderModel

    var body: some View {
        VStack(spacing: 0) {
            Picker("", selection: $reader.aiTab) {
                ForEach(AITab.allCases) { tab in
                    Text(tabTitle(tab)).tag(tab)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(.horizontal, 12)
            .padding(.top, 10)
            .padding(.bottom, 8)
            .accessibilityIdentifier("booklight.ai.tabs")
            Divider().overlay(Theme.border)
            pane
        }
        .background(Theme.sidebar)
    }

    private func tabTitle(_ tab: AITab) -> String {
        switch tab {
        case .translate: return L10n.t("ui.tab.translate")
        case .summarize: return L10n.t("ui.tab.summarize")
        case .chat: return L10n.t("ui.tab.chat")
        }
    }

    @ViewBuilder
    private var pane: some View {
        switch reader.aiTab {
        case .translate:
            CardListPane(actionTitle: reader.epubActive ? L10n.t("ui.epub.translate-page") : L10n.t("ui.translate.page"),
                         action: { reader.translateCurrentPage() },
                         cards: reader.translateCards,
                         emptyHint: L10n.t("ui.welcome.feature1"))
        case .summarize:
            SummarizePane(reader: reader)
        case .chat:
            ChatPane(reader: reader)
        }
    }
}

// MARK: - translate / summarize card lists

struct CardListPane: View {
    let actionTitle: String
    let action: () -> Void
    let cards: [StreamCard]
    let emptyHint: String

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button(action: action) {
                    Label(actionTitle, systemImage: "globe")
                        .font(.system(size: 12, weight: .medium))
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .tint(Theme.accent)
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            cardList
        }
    }

    @ViewBuilder
    private var cardList: some View {
        if cards.isEmpty {
            EmptyHint(emptyHint)
        } else {
            ScrollView {
                LazyVStack(spacing: 10) {
                    ForEach(cards) { card in
                        StreamCardView(card: card)
                    }
                }
                .padding(.horizontal, 10)
                .padding(.bottom, 12)
            }
        }
    }
}

struct EmptyHint: View {
    let text: String
    var symbol: String = "sparkles"

    init(_ text: String, symbol: String = "sparkles") {
        self.text = text
        self.symbol = symbol
    }

    var body: some View {
        ContentUnavailableView {
            Label(L10n.t("ui.toolbar.ai"), systemImage: symbol)
        } description: {
            Text(text)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct SummarizePane: View {
    @ObservedObject var reader: ReaderModel

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                pillButton(reader.epubActive ? L10n.t("ui.epub.summarize-page") : L10n.t("ui.summarize.page")) { reader.summarizePage() }
                pillButton(L10n.t("ui.summarize.selection")) { reader.summarizeSelection() }
                pillButton(L10n.t("ui.summarize.doc")) { reader.summarizeDoc() }
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            if reader.summarizeCards.isEmpty {
                EmptyHint(L10n.t("ui.welcome.feature1"))
            } else {
                ScrollView {
                    LazyVStack(spacing: 10) {
                        ForEach(reader.summarizeCards) { card in
                            StreamCardView(card: card)
                        }
                    }
                    .padding(.horizontal, 10)
                    .padding(.bottom, 12)
                }
            }
        }
    }

    private func pillButton(_ title: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 12))
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
        .tint(Theme.accent)
    }
}

// MARK: - chat

struct ChatPane: View {
    @ObservedObject var reader: ReaderModel
    @State private var draft = ""

    var body: some View {
        VStack(spacing: 0) {
            scopeRow
            Divider().overlay(Theme.border)
            messageList
            Divider().overlay(Theme.border)
            inputRow
        }
    }

    private var scopeRow: some View {
        HStack(spacing: 8) {
            Picker("", selection: $reader.chatScope) {
                Text(reader.epubActive ? L10n.t("ui.epub.scope-page") : L10n.t("ui.chat.scope.page")).tag(0)
                Text(L10n.t("ui.chat.scope.selection")).tag(1)
                Text(L10n.t("ui.chat.scope.doc")).tag(2)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .controlSize(.small)
            .accessibilityLabel(L10n.t("ui.chat.scope"))
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 10) {
                    if reader.chatBubbles.isEmpty {
                        ContentUnavailableView {
                            Label(L10n.t("ui.tab.chat"), systemImage: "bubble.left.and.bubble.right")
                        } description: {
                            Text(L10n.t("ui.chat.placeholder"))
                        }
                        .frame(minHeight: 220)
                    }
                    ForEach(reader.chatBubbles) { bubble in
                        ChatBubbleView(bubble: bubble)
                            .id(bubble.id)
                    }
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
            }
            .onChange(of: reader.chatBubbles.count) { _, _ in
                if let last = reader.chatBubbles.last {
                    withAnimation(.easeOut(duration: 0.15)) {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
        }
    }

    private var inputRow: some View {
        HStack(alignment: .bottom, spacing: 8) {
            TextField(L10n.t("ui.chat.placeholder"), text: $draft, axis: .vertical)
                .textFieldStyle(.plain)
                .lineLimit(1...4)
                .font(.system(size: 13))
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                        .fill(Theme.panel)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                                .stroke(Theme.border, lineWidth: 0.5)))
                .onSubmit { send() }

            if reader.chatBusy {
                Button {
                    reader.stopChat()
                } label: {
                    Image(systemName: "stop.fill")
                        .font(.system(size: 11, weight: .bold))
                        .frame(width: 26, height: 26)
                }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.circle)
                .tint(Theme.danger)
                .help(L10n.t("ui.chat.stop"))
            } else {
                Button {
                    send()
                } label: {
                    Image(systemName: "arrow.up")
                        .font(.system(size: 12, weight: .bold))
                        .frame(width: 26, height: 26)
                }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.circle)
                .tint(Theme.accent)
                .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty)
                .help(L10n.t("ui.chat.send"))
            }
        }
        .padding(10)
    }

    private func send() {
        let question = draft
        guard !question.trimmingCharacters(in: .whitespaces).isEmpty, !reader.chatBusy else { return }
        draft = ""
        reader.sendChat(question)
    }
}

struct ChatBubbleView: View {
    let bubble: ChatBubble

    var body: some View {
        HStack {
            if bubble.isUser {
                Spacer(minLength: 36)
                Text(bubble.text)
                    .font(.system(size: 13))
                    .textSelection(.enabled)
                    .padding(.horizontal, 11)
                    .padding(.vertical, 7)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                            .fill(Theme.accentSoft))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                            .stroke(Theme.accentBorder, lineWidth: 0.5))
            } else if let card = bubble.card {
                StreamCardView(card: card)
            } else {
                Text(bubble.text)
                    .font(.system(size: 13))
                    .foregroundStyle(bubble.dimmed ? Theme.textFaint : Theme.text)
                    .italic(bubble.dimmed)
                    .textSelection(.enabled)
                    .padding(.horizontal, 11)
                    .padding(.vertical, 7)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                            .fill(Theme.panel))
                Spacer(minLength: 36)
            }
        }
        .frame(maxWidth: .infinity, alignment: bubble.isUser ? .trailing : .leading)
    }
}

// MARK: - notes (annotations + bookmarks — the sidebar's third mode)

struct NotesPane: View {
    @ObservedObject var tab: PDFTab
    @ObservedObject var reader: ReaderModel
    let onExport: () -> Void

    private var hasAnything: Bool {
        !tab.docData.annotations.isEmpty || !tab.docData.bookmarks.isEmpty
    }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 10) {
                AnnotationSaveBanner(reader: reader, onExport: onExport)
                if reader.pendingUndo != nil {
                    undoBanner
                }
                if hasAnything {
                    HStack(alignment: .firstTextBaseline) {
                        Text(L10n.t("ui.sidebar.annotations"))
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(Theme.textDim)
                        Spacer()
                        Button(action: onExport) {
                            Label(L10n.t("ui.notes.export"), systemImage: "square.and.arrow.up")
                                .font(.caption)
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(Theme.accent)
                    }
                    .padding(.top, 2)
                }
                if tab.docData.annotations.isEmpty {
                    ContentUnavailableView {
                        Label(L10n.t("ui.notes.empty"), systemImage: "highlighter")
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.top, 8)
                } else {
                    ForEach(sortedAnnotations) { annotation in
                        NoteCard(annotation: annotation, reader: reader)
                    }
                }
                bookmarkSection
            }
            .padding(10)
        }
    }

    private var undoBanner: some View {
        HStack(spacing: 8) {
            Text(L10n.t("ui.notes.deleted"))
                .font(.caption)
                .foregroundStyle(Theme.textDim)
            Spacer()
            Button(L10n.t("ui.undo")) {
                reader.undoDeleteAnnotation()
            }
            .font(.caption.weight(.semibold))
            .buttonStyle(.plain)
            .foregroundStyle(Theme.accent)
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(Theme.panel))
    }

    private var sortedAnnotations: [StoredAnnotation] {
        tab.docData.annotations.sorted {
            $0.page == $1.page ? $0.created < $1.created : $0.page < $1.page
        }
    }

    @ViewBuilder
    private var bookmarkSection: some View {
        Text(L10n.t("ui.notes.bookmarks"))
            .font(.caption.weight(.semibold))
            .foregroundStyle(Theme.textDim)
            .padding(.top, 6)
        if tab.docData.bookmarks.isEmpty {
            Text(L10n.t("ui.notes.bookmark-empty"))
                .font(.caption)
                .foregroundStyle(Theme.textFaint)
                .padding(.vertical, 2)
        }
        ForEach(tab.docData.bookmarks.sorted { $0.page < $1.page }) { bookmark in
            HStack(spacing: 10) {
                Image(systemName: "bookmark.fill")
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.accent)
                    .frame(width: 18)
                VStack(alignment: .leading, spacing: 1) {
                    Text(bookmark.label.isEmpty
                         ? L10n.t("ui.notes.page", "\(bookmark.page)")
                         : bookmark.label)
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.text)
                        .lineLimit(1)
                    Text(L10n.t("ui.notes.page", "\(bookmark.page)"))
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.textFaint)
                }
                Spacer()
                Button {
                    reader.removeBookmark(bookmark.page)
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundStyle(Theme.textFaint)
                }
                .buttonStyle(.plain)
                .help(L10n.t("ui.notes.bookmark-remove"))
                .accessibilityLabel(L10n.t("ui.notes.bookmark-remove"))
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.small, style: .continuous)
                    .fill(Theme.panel))
            .contentShape(Rectangle())
            .onTapGesture { tab.goToPage(bookmark.page) }
        }
    }
}

struct AnnotationSaveBanner: View {
    @ObservedObject var reader: ReaderModel
    let onExport: () -> Void

    var body: some View {
        switch reader.annotationPersistence {
        case .saved:
            EmptyView()
        case .pending:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text(L10n.t("ui.notes.save-pending"))
                    .font(.caption)
                    .foregroundStyle(Theme.textDim)
                Spacer(minLength: 0)
            }
            .padding(10)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                    .fill(Theme.panel))
        case .recovered:
            recoveryCard(L10n.t("ui.notes.recovery-pending"))
        case .failed(let message):
            recoveryCard(L10n.t("ui.notes.save-failed", message))
        }
    }

    private func recoveryCard(_ message: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label {
                Text(message)
                    .font(.caption)
                    .foregroundStyle(Theme.danger)
                    .fixedSize(horizontal: false, vertical: true)
            } icon: {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(Theme.danger)
            }
            HStack(spacing: 14) {
                Button(L10n.t("ui.notes.retry-save")) {
                    reader.retryAnnotationSave()
                }
                Button(L10n.t("ui.notes.export"), action: onExport)
            }
            .buttonStyle(.plain)
            .font(.caption.weight(.semibold))
            .foregroundStyle(Theme.accent)
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(Theme.danger.opacity(0.07)))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .stroke(Theme.danger.opacity(0.28), lineWidth: 0.5))
    }
}

struct NoteCard: View {
    let annotation: StoredAnnotation
    @ObservedObject var reader: ReaderModel
    @State private var hovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 7) {
                Circle()
                    .fill(Theme.annotationSwiftUIColor(annotation.color))
                    .frame(width: 8, height: 8)
                Text(L10n.t("ui.notes.page", "\(annotation.page)"))
                    .font(.caption2.monospacedDigit().weight(.medium))
                    .foregroundStyle(Theme.textDim)
                if annotation.kind != "highlight" {
                    Text(L10n.t("ui.kind.\(annotation.kind)"))
                        .font(.caption2)
                        .foregroundStyle(Theme.textFaint)
                }
                Spacer(minLength: 0)
                menu
                    .opacity(hovering ? 1 : 0.45)
            }
            Text(annotation.excerpt)
                .font(.system(size: 12))
                .foregroundStyle(Theme.text)
                .lineLimit(4)
                .fixedSize(horizontal: false, vertical: true)
            if !annotation.note.isEmpty {
                Text(annotation.note)
                    .font(.caption)
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(3)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(hovering ? Theme.panelAlt : Theme.panel))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture { reader.pdf?.goToPage(annotation.page) }
        .contextMenu { actions }
    }

    private var menu: some View {
        Menu {
            actions
        } label: {
            Image(systemName: "ellipsis")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Theme.textDim)
                .frame(width: 18, height: 18)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
    }

    @ViewBuilder
    private var actions: some View {
        Button(L10n.t("ui.menu.translate")) {
            reader.translateSelectionText(annotation.excerpt)
        }
        Divider()
        Button(L10n.t("ui.notes.delete-annotation"), role: .destructive) {
            reader.removeAnnotation(annotation.id)
        }
    }
}
