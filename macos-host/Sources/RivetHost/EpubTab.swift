import SwiftUI
import WebKit
import RivetRuntime

/// An open EPUB: metadata from the domain core plus the WKWebView that
/// renders the whole book (one load, continuous scroll — v1 epub.ts model).
/// Business (parse/sanitize/text) lives in the Racket backend; this class
/// only drives presentation state.
@MainActor
final class EpubTab: ObservableObject, Identifiable {
    let id = UUID()
    let path: String
    @Published var title: String
    weak var reader: ReaderModel?

    @Published var chapters: Int = 0
    @Published var currentPage: Int = 1        // chapter, kept for recents
    @Published var fontScale: Double = 1.0
    @Published var toc: [RivetTypes.EpubTocItem] = []
    @Published var loaded = false
    /// all chapter texts prefetched (AI / TTS / whole-book search can read)
    @Published var textReady = false

    var recentTask: Task<Void, Never>?

    /// A text selection with its anchor: character offsets inside the
    /// chapter's plain text (the same text the search uses), computed by
    /// the page script on mouseup. Consumed by the AI selection actions
    /// and by annotation adds.
    struct Selection {
        let text: String
        let chapter: Int
        let start: Int
        let end: Int
    }

    /// Last selection relayed from the web view (mouseup bridge, v1
    /// onSelection). Consumed by the AI selection actions and by the
    /// EPUB annotation flow.
    var pendingSelection: Selection?

    /// chapter-text cache for search + repeated AI calls
    var textCache: [Int: String] = [:]
    var searchHits: [(chapter: Int, range: Range<String.Index>)] = []

    /// Annotations + bookmarks, same JSON shape as the PDF store (page =
    /// chapter ordinal; EPUB annotations carry the chapter/offset anchor).
    @Published var docData = DocData.empty
    var annoTask: Task<Void, Never>?
    var annotationsLoaded = false
    /// Marks are injected after the page finished loading.
    var marksApplied = false

    /// last theme pushed into the page ("light"/"dark") so the view layer
    /// only re-applies on system appearance changes
    var appliedTheme: String = ""

    let webView: WKWebView
    var navDelegate: WKNavigationDelegate?

    init(path: String, title: String, view: WKWebView, reader: ReaderModel) {
        self.path = path
        self.title = title
        self.webView = view
        self.reader = reader
    }

    /// Strong ownership: WKWebView's delegate property is weak and a
    /// temporary would deallocate before didFinish fires.
    func installDelegates() {
        let delegate = EpubNavDelegate(tab: self)
        navDelegate = delegate
        webView.navigationDelegate = delegate
        webView.configuration.userContentController.add(
            EpubBridgeHandler(tab: self), name: "bridge")
    }

    /// Whole-book HTML assembled from sanitized chapter bodies, wrapped in
    /// the v1 reading typography; chapters are addressable sections.
    func loadBook(api: RivetAPI) {
        Task {
            do {
                let view = try await api.epub_open(path: path)
                if !view.title.isEmpty {
                    self.title = view.title
                    self.reader?.objectWillChange.send()
                }
                self.chapters = Int(view.chapters)
                self.toc = view.toc
                var sections: [String] = []
                sections.append("<div class=\"epub-book-title\"><div class=\"book-title-text\">\(Self.escapeHTML(view.title))</div></div>")
                for i in 1...max(1, Int(view.chapters)) {
                    let body = try await api.epub_chapter_html(path: path, index: Int64(i))
                    sections.append(
                        "<div class=\"epub-chapter\" data-chapter=\"\(i)\">\(String(decoding: body, as: UTF8.self))</div>")
                    // pull the text in the same tight sequence — spaced-out
                    // requests on this channel can stall (see rivet issue)
                    if self.textCache[Int(i)] == nil {
                        self.textCache[Int(i)] = (try? await api.epub_chapter_text(path: path, index: Int64(i))) ?? ""
                    }
                }
                await MainActor.run { self.textReady = true }
                let initialTheme = self.reader?.app?.readingSurface() ?? Self.systemTheme()
                let html = Self.pageHTML(sections: sections, theme: initialTheme)
                self.installDelegates()
                self.webView.loadHTMLString(html, baseURL: nil)
            } catch let e as ClientError {
                reader?.alert(L10n.t("ui.pages.edit-failed", Self.describe(e)))
            } catch {
                reader?.alert(L10n.t("ui.pages.edit-failed", String(describing: error)))
            }
        }
    }

    static func describe(_ e: ClientError) -> String {
        if case .backend(let message) = e { return message }
        return String(describing: e)
    }

    /// The reading page follows the system appearance (no in-app theme
    /// toggle — Apple form); the reader re-applies this on view updates.
    static func systemTheme() -> String {
        NSApp.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            ? "dark" : "light"
    }

    // MARK: presentation state pushed into the page

    func applyFontScale(_ scale: Double) {
        fontScale = scale
        webView.evaluateJavaScript(
            "document.documentElement.style.setProperty('--fs', \(scale))", completionHandler: nil)
    }

    func applyTheme(_ theme: String) {
        webView.evaluateJavaScript(
            "document.documentElement.setAttribute('data-theme', \(Self.jsString(theme)))",
            completionHandler: nil)
    }

    func goToChapter(_ chapter: Int) {
        currentPage = max(1, min(chapter, chapters))
        webView.evaluateJavaScript(
            """
            (() => {
              const el = document.querySelectorAll('.epub-chapter')[\(currentPage - 1)];
              if (el) el.scrollIntoView({ block: 'start' });
            })()
            """, completionHandler: nil)
    }

    /// Jump to the nth occurrence of `query` (whole-book search, v1 gotoHit):
    /// chapter index comes from the host-side text search, in-page highlight
    /// via the marks script.
    func highlightHit(chapter: Int, ordinal: Int, query: String) {
        goToChapter(chapter)
        webView.evaluateJavaScript(
            """
            (() => {
              const root = document.querySelectorAll('.epub-chapter')[\(chapter - 1)];
              if (!root) return;
              clearMarks(root);
              markOccurrence(root, \(Self.jsString(query)), \(ordinal));
              const mark = root.querySelector('mark.epub-hit');
              if (mark) mark.scrollIntoView({ block: 'center' });
            })()
            """, completionHandler: nil)
    }

    func clearHighlights() {
        webView.evaluateJavaScript(
            "document.querySelectorAll('.epub-chapter').forEach(el => clearMarks(el));",
            completionHandler: nil)
    }

    // MARK: annotations (same JSON store as PDFs; page = chapter ordinal)

    func queueAnnotationSave() {
        reader?.queueEpubAnnotationSave(self)
    }

    func storeAnnotationData() async {
        await reader?.storeEpubAnnotationData(self)
    }

    func loadAnnotations(api: RivetAPI, sidecar: Bool) {
        Task { [weak self] in
            guard let self else { return }
            if let data = try? await api.get_annotations(
                pdf_path: path, sidecar: sidecar),
               let decoded = try? JSONDecoder().decode(DocData.self, from: data) {
                self.docData = decoded
                self.annotationsLoaded = true
                self.applyAnnotationsScript()
                let recovered = (try? await api.annotation_recovery_pending(
                    pdf_path: path, sidecar: sidecar)) ?? false
                self.reader?.annotationLoadFinished(recovered: recovered)
            }
        }
    }

    /// (Re)inject every stored annotation into the page (no-op until the
    /// page finished loading).
    func applyAnnotationsScript() {
        guard loaded else { return }
        guard let data = try? JSONEncoder().encode(docData),
              let json = String(data: data, encoding: .utf8) else { return }
        webView.evaluateJavaScript(
            "const d = JSON.parse(\(Self.jsString(json))); applyEpubAnnotations(d.annotations);",
            completionHandler: nil)
    }

    // MARK: static page assets

    static func escapeHTML(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }

    static func jsString(_ s: String) -> String {
        let escaped = s.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n")
        return "\"\(escaped)\""
    }

    /// v1 READING_CSS typography + theme; the marks helpers power search.
    static func pageHTML(sections: [String], theme: String) -> String {
        let css = """
        :root { --fs: 1; }
        :root[data-theme="light"] { --fg: #292524; --bg: #F5F2ED; }
        :root[data-theme="sepia"] { --fg: #57534E; --bg: #EFE6D5; }
        :root[data-theme="dark"] { --fg: #D6D3D1; --bg: #171412; }
        html { background: var(--bg); }
        body { margin: 0; padding: 0 0 40vh; color: var(--fg);
          font-family: "New York", "Iowan Old Style", Georgia,
            "Source Han Serif SC", "Noto Serif SC", "Songti SC", serif;
          font-size: calc(17px * var(--fs)); line-height: 1.75;
          -webkit-font-smoothing: antialiased; }
        p { margin: 0 0 1.05em; text-align: left; text-wrap: pretty; }
        h1, h2, h3, h4 { line-height: 1.3; margin: 1.6em 0 0.55em;
          font-weight: 600; letter-spacing: -0.01em; }
        h1 { font-size: 1.9em; }
        h2 { font-size: 1.45em; }
        h3 { font-size: 1.18em; }
        img, svg { max-width: 100%; height: auto; }
        blockquote { margin: 1.2em 0; padding-left: 1em;
          border-left: 3px solid #8884; color: #888; }
        a { color: inherit; pointer-events: none; }
        ::selection { background: rgba(196, 98, 62, 0.22); }
        .epub-book-title { max-width: 38rem; margin: 0 auto;
          padding: 16vh 28px 8vh; text-align: center; }
        .book-title-text { font-size: 2.1em; font-weight: 600;
          letter-spacing: -0.02em; }
        .epub-chapter { max-width: 38rem; margin: 0 auto; padding: 0 28px; }
        mark.epub-hit { background: #FFD500; color: inherit; border-radius: 2px; }
        """
        let script = """
        function clearMarks(root) {
          const marks = root.querySelectorAll('mark.epub-hit');
          marks.forEach(m => {
            const parent = m.parentNode;
            parent.replaceChild(document.createTextNode(m.textContent), m);
            parent.normalize();
          });
        }
        function markOccurrence(root, needle, ordinal) {
          let count = 0;
          const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
          const nodes = [];
          while (walker.nextNode()) nodes.push(walker.currentNode);
          for (const node of nodes) {
            const lower = node.nodeValue.toLowerCase();
            let from = 0, at;
            while ((at = lower.indexOf(needle.toLowerCase(), from)) !== -1) {
              if (count === ordinal) {
                const range = document.createRange();
                range.setStart(node, at);
                range.setEnd(node, at + needle.length);
                const mark = document.createElement('mark');
                mark.className = 'epub-hit';
                try { range.surroundContents(mark); } catch (e) {}
                return;
              }
              count += 1;
              from = at + needle.length;
            }
          }
        }
        window.addEventListener('mouseup', () => {
          const sel = window.getSelection();
          if (!sel || sel.isCollapsed || sel.rangeCount === 0) return;
          const text = sel.toString().trim();
          if (text.length < 2) return;
          // Anchor: character offsets within the start chapter's plain
          // text (its textContent), so a mark survives reloads without
          // fragile DOM paths.
          const range = sel.getRangeAt(0);
          const root = range.startContainer.nodeType === 1
            ? range.startContainer
            : range.startContainer.parentNode;
          const chapterEl = root && root.closest
            ? root.closest('.epub-chapter') : null;
          if (!chapterEl) return;
          const sections = Array.from(document.querySelectorAll('.epub-chapter'));
          const chapter = sections.indexOf(chapterEl) + 1;
          if (chapter < 1) return;
          const walker = document.createTreeWalker(chapterEl, NodeFilter.SHOW_TEXT);
          let start = -1, end = -1, offset = 0, node;
          while ((node = walker.nextNode())) {
            const len = node.nodeValue.length;
            if (start < 0 && node === range.startContainer) start = offset + range.startOffset;
            if (node === range.endContainer) end = offset + range.endOffset;
            offset += len;
          }
          if (start < 0 || end < 0 || end <= start) return;
          window.webkit.messageHandlers.bridge.postMessage(
            JSON.stringify({ kind: 'selection', text: text.slice(0, 800),
                             chapter: chapter, start: start, end: end }));
        });

        function textNodeMap(root) {
          const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
          const nodes = [];
          let total = 0, node;
          while ((node = walker.nextNode())) { nodes.push(node); total += node.nodeValue.length; }
          return { nodes, total };
        }

        function styleFor(kind, color) {
          if (kind === 'underline') return 'text-decoration: underline; text-decoration-color:' + markColor(color) + ';';
          if (kind === 'strike') return 'text-decoration: line-through; text-decoration-color:' + markColor(color) + ';';
          const alpha = { yellow: '0.45', green: '0.35', blue: '0.28' }[color] || '0.4';
          return 'background:' + hexToRgba(markColor(color), alpha) + ';';
        }
        function markColor(color) {
          return { yellow: '#FFD500', green: '#4CC383', blue: '#569CFF' }[color] || '#FFD500';
        }
        function hexToRgba(hex, alpha) {
          const v = parseInt(hex.slice(1), 16);
          return 'rgba(' + ((v >> 16) & 255) + ',' + ((v >> 8) & 255) + ',' + (v & 255) + ',' + alpha + ')';
        }

        // One annotation may wrap several text-node segments (each gets
        // its own mark with the same data-id — the PDF multi-rect model).
        function applyEpubAnnotation(id, chapter, start, end, kind, color) {
          const root = document.querySelectorAll('.epub-chapter')[chapter - 1];
          if (!root) return;
          removeEpubAnnotation(id);
          const map = textNodeMap(root);
          if (start < 0 || end > map.total) return;  // offsets drifted
          let offset = 0;
          for (const node of map.nodes) {
            const len = node.nodeValue.length;
            const from = Math.max(0, start - offset);
            const to = Math.min(len, end - offset);
            if (to > from) {
              const range = document.createRange();
              range.setStart(node, from);
              range.setEnd(node, to);
              const mark = document.createElement('mark');
              mark.className = 'epub-anno';
              mark.dataset.id = id;
              mark.setAttribute('style', styleFor(kind, color));
              try { range.surroundContents(mark); } catch (e) {}
            }
            offset += len;
          }
        }

        function applyEpubAnnotations(list) {
          (list || []).forEach(a =>
            applyEpubAnnotation(a.id, a.anchor.chapter, a.anchor.start,
                                a.anchor.end, a.kind, a.color));
        }

        function removeEpubAnnotation(id) {
          document.querySelectorAll('mark.epub-anno[data-id="' + id + '"]').forEach(m => {
            const parent = m.parentNode;
            parent.replaceChild(document.createTextNode(m.textContent), m);
            parent.normalize();
          });
        }
        window.addEventListener('scroll', () => {
          const sections = document.querySelectorAll('.epub-chapter');
          const line = window.scrollY + window.innerHeight * 0.35;
          let current = 1;
          sections.forEach((el, i) => { if (el.offsetTop <= line) current = i + 1; });
          window.webkit.messageHandlers.bridge.postMessage(
            JSON.stringify({ kind: 'chapter', chapter: current }));
        });
        """
        let themeAttr = theme == "dark" ? "dark" : (theme == "light" ? "light" : "light")
        return """
        <!doctype html><html data-theme="\(themeAttr)"><head><meta charset="utf-8">
        <style>\(css)</style><script>\(script)</script></head>
        <body>\(sections.joined(separator: "\n"))</body></html>
        """
    }

    /// Retain cycles between the WKWebView config and the tab must be broken
    /// when the tab closes.
    func teardown() {
        webView.configuration.userContentController.removeAllScriptMessageHandlers()
        webView.navigationDelegate = nil
    }
}

extension EpubTab {
    /// Held strongly by the tab — WKWebView's delegate property is weak and
    /// an unowned temporary would deallocate before didFinish fires.
    final class EpubNavDelegate: NSObject, WKNavigationDelegate {
        weak var tab: EpubTab?
        init(tab: EpubTab) { self.tab = tab }
        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            tab?.loaded = true
            // The persisted scale rides in with the tab; the CSS variable
            // only exists once the page is up.
            if let tab, tab.fontScale != 1.0 {
                tab.applyFontScale(tab.fontScale)
            }
            tab?.applyAnnotationsScript()
        }
    }
}

/// Hosts the tab's long-lived WKWebView. The representable only re-attaches
/// the view and keeps the page theme in step with the system appearance.
struct EpubViewport: NSViewRepresentable {
    @ObservedObject var tab: EpubTab

    func makeNSView(context: Context) -> WKWebView {
        tab.webView
    }

    func updateNSView(_ view: WKWebView, context: Context) {
        // The page surface follows the reading theme (paper/night) and only
        // falls back to the app appearance under "system".
        let theme = tab.reader?.app?.readingSurface() ?? EpubTab.systemTheme()
        if theme != tab.appliedTheme {
            tab.appliedTheme = theme
            tab.applyTheme(theme)
        }
    }
}

extension EpubTab {
    static func scriptHandler(for tab: EpubTab) -> WKScriptMessageHandler {
        EpubBridgeHandler(tab: tab)
    }

    final class EpubBridgeHandler: NSObject, WKScriptMessageHandler {
        weak var tab: EpubTab?
        init(tab: EpubTab) { self.tab = tab }

        func userContentController(_ userContentController: WKUserContentController,
                                   didReceive message: WKScriptMessage) {
            guard let body = message.body as? String,
                  let dataObj = body.data(using: .utf8),
                  let obj = try? JSONSerialization.jsonObject(with: dataObj) as? [String: Any]
            else { return }
            Task { @MainActor in
                guard let tab = self.tab else { return }
                switch obj["kind"] as? String {
                case "selection":
                    if let text = obj["text"] as? String {
                        tab.pendingSelection = EpubTab.Selection(
                            text: text,
                            chapter: obj["chapter"] as? Int ?? tab.currentPage,
                            start: obj["start"] as? Int ?? 0,
                            end: obj["end"] as? Int ?? 0)
                    }
                case "chapter":
                    if let ch = obj["chapter"] as? Int {
                        tab.currentPage = ch
                        tab.reader?.noteEpubChapter(tab)
                    }
                default:
                    break
                }
            }
        }
    }
}
