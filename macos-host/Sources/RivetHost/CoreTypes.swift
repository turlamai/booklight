import SwiftUI
import AppKit
import PDFKit

// Shared host-side types: annotation storage schema, streaming cards, the
// PDFView subclass and small helpers. Document/window state lives in
// AppModel (process-wide) and ReaderModel (per window).

// MARK: - annotation storage types (JSON schema of racket/pdfgist/annotations.rkt)

struct AnnoRect: Codable, Equatable {
    var x: Double
    var y: Double
    var width: Double
    var height: Double

    var nsRect: NSRect { NSRect(x: x, y: y, width: width, height: height) }
    init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x; self.y = y; self.width = width; self.height = height
    }
    init(_ rect: NSRect) {
        x = Double(rect.minX); y = Double(rect.minY)
        width = Double(rect.width); height = Double(rect.height)
    }
}

/// EPUB annotations anchor to chapter + character offsets inside the
/// chapter's plain text (the same text the search uses); PDF annotations
/// keep page-space rects and have no anchor.
struct StoredAnchor: Codable, Equatable {
    var chapter: Int
    var start: Int
    var end: Int
}

struct StoredAnnotation: Codable, Identifiable, Equatable {
    var id: String
    var page: Int
    var rects: [AnnoRect]
    var excerpt: String
    var color: String
    var kind: String
    var note: String
    var created: Int
    var anchor: StoredAnchor?
}

struct StoredBookmark: Codable, Identifiable, Equatable {
    var page: Int
    var label: String
    var created: Int
    var id: Int { page }
}

struct DocData: Codable, Equatable {
    var annotations: [StoredAnnotation]
    var bookmarks: [StoredBookmark]
    static let empty = DocData(annotations: [], bookmarks: [])
}

// MARK: - page edit ops

/// In-memory like v1: the source file on disk is never touched; extract/bake
/// go through a save panel.
enum PageEditOp {
    case deleteCurrent
    case rotateCurrent
    case insertBlankAfterCurrent
    case extractCurrent
    case extract(pages: [Int64])
    case merge(path: String)
}

// MARK: - streaming card

/// One AI stream: buffers reasoning/content per the v1 StreamBuffer semantics —
/// reasoning accumulates until the first answer delta; a reasoning-only stream
/// ends as dimmed reasoning; nothing at all becomes （已停止）/（无返回内容）.
@MainActor
final class StreamCard: ObservableObject, Identifiable {
    enum Phase: Equatable {
        case streaming
        case done
        case empty(stopped: Bool)
        case error(String)
        case notice(String)
    }

    let id = UUID()
    var requestId: Int64
    let title: String
    let meta: String
    let sourceText: String
    @Published var reasoning = ""
    @Published var content = ""
    @Published var phase: Phase = .streaming
    /// Set by the owning reader for chat cards: fired once when the stream
    /// settles, so chat busy-state and history bookkeeping can finalize.
    var onFinish: ((StreamCard) -> Void)?

    init(requestId: Int64 = -1, title: String, meta: String = "", sourceText: String = "") {
        self.requestId = requestId
        self.title = title
        self.meta = meta
        self.sourceText = sourceText
    }

    static func makeNotice(_ message: String) -> StreamCard {
        let card = StreamCard(title: "")
        card.phase = .notice(message)
        return card
    }

    func push(_ delta: String, reasoning isReasoning: Bool) {
        if isReasoning {
            if content.isEmpty { reasoning += delta }
        } else {
            content += delta
        }
    }

    func finish(stopped: Bool = false) {
        if content.isEmpty && reasoning.isEmpty {
            phase = .empty(stopped: stopped)
        } else {
            phase = .done
        }
    }

    func fail(_ message: String) {
        phase = .error(message)
    }

    var copyText: String {
        if !content.isEmpty { return content }
        return reasoning
    }
}

struct ChatBubble: Identifiable {
    let id = UUID()
    let isUser: Bool
    var text: String
    var dimmed: Bool
    var card: StreamCard?

    static func user(_ text: String) -> ChatBubble {
        ChatBubble(isUser: true, text: text, dimmed: false, card: nil)
    }
}

struct PasswordPrompt: Identifiable {
    let id = UUID()
    let wrong: Bool
    let continuation: CheckedContinuation<String?, Never>
}

// MARK: - AI inspector tabs (annotations moved to the reader sidebar)

enum AITab: String, CaseIterable, Identifiable {
    case translate, summarize, chat
    var id: String { rawValue }
}

// MARK: - outline

struct OutlineNode: Identifiable {
    let id = UUID()
    let label: String
    let destination: PDFDestination?
    let children: [OutlineNode]
}

// MARK: - AppKit helpers

/// NSMenuItem that fires a closure (no responder-chain plumbing).
@MainActor
final class BlockMenuItem: NSMenuItem {
    @MainActor private let handler: () -> Void

    init(title: String, handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(fire), keyEquivalent: "")
        target = self
    }

    @available(*, unavailable)
    required init(coder: NSCoder) {
        fatalError("not supported")
    }

    @objc private func fire() { handler() }
}

/// PDFView subclass offering the annotation/translate context menu whenever a
/// text selection exists.
final class GistPDFView: PDFView {
    weak var hostTab: PDFTab?

    override func menu(for event: NSEvent) -> NSMenu? {
        guard currentSelection != nil, let tab = hostTab else { return super.menu(for: event) }
        return tab.selectionMenu()
    }

    override func mouseDown(with event: NSEvent) {
        super.mouseDown(with: event)
        if currentSelection == nil {
            hostTab?.reader?.selectionCleared()
        }
    }
}

// MARK: - night content inversion (mac PDFKit leg; Windows/Linux invert
// inside their PDFium render pipelines)

/// A PDFPage that can draw itself through a difference-blend white pass —
/// the PDF.js-style negative filter. The flag is app-wide (the reading
/// surface is a setting, not per-document) and only consulted at draw
/// time, so a flip needs a fresh document (see InvertedPDFDocument users).
final class InvertedPDFPage: PDFPage {
    // Draw-time read only; flips happen on the main actor (applyCanvasSurface)
    // before any fresh document draws — same pattern as L10n.language.
    nonisolated(unsafe) static var isInverted = false

    override func draw(with box: PDFDisplayBox, to context: CGContext) {
        super.draw(with: box, to: context)
        guard Self.isInverted else { return }
        context.saveGState()
        // difference with white inverts everything drawn so far: paper goes
        // dark, text goes light — same semantics as the PDFium negative
        // filter on Windows/Linux.
        context.setBlendMode(.difference)
        context.setFillColor(CGColor(gray: 1, alpha: 1))
        context.fill(bounds(for: box))
        context.restoreGState()
    }
}

/// A PDFDocument whose pages are InvertedPDFPage instances.
final class InvertedPDFDocument: PDFDocument {
    override var pageClass: AnyClass { InvertedPDFPage.self }
}

// MARK: - target-language display names (shared by reader + settings)

func languageName(_ code: RivetTypes.TargetLanguage) -> String {
    switch code {
    case .zh: return "中文"
    case .zh_hant: return "繁體中文"
    case .en: return "English"
    case .ja: return "日本語"
    case .ko: return "한국어"
    case .fr: return "Français"
    case .de: return "Deutsch"
    case .es: return "Español"
    }
}
