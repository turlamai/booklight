import SwiftUI
import AppKit

// MARK: - markdown-lite renderer (subset: headings, lists, quotes, code, bold/italic/code)

enum MarkdownLite {
    static func attributed(_ text: String) -> AttributedString {
        AttributedString(render(text))
    }

    static func render(_ text: String) -> NSAttributedString {
        let bodyFont = NSFont.systemFont(ofSize: 13)
        let bodyColor = NSColor.labelColor
        let base: [NSAttributedString.Key: Any] = [.font: bodyFont, .foregroundColor: bodyColor]

        let out = NSMutableAttributedString()
        var inCode = false
        var codeLines: [String] = []

        for rawLine in text.components(separatedBy: "\n") {
            if rawLine.hasPrefix("```") {
                if inCode {
                    out.append(codeBlock(codeLines))
                    codeLines = []
                    inCode = false
                } else {
                    inCode = true
                }
                continue
            }
            if inCode {
                codeLines.append(rawLine)
                continue
            }
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty {
                out.append(NSAttributedString(string: "\n", attributes: base))
                continue
            }
            if line.count >= 3 && line.allSatisfy({ $0 == "-" || $0 == "=" || $0 == "*" || $0 == "_" }) {
                let hr = NSAttributedString(
                    string: String(repeating: "—", count: 12) + "\n",
                    attributes: [.font: bodyFont, .foregroundColor: NSColor.tertiaryLabelColor])
                out.append(hr)
                continue
            }
            if let heading = headingAttributes(line) {
                out.append(inline(String(line.dropFirst(heading.level)), font: heading.font, color: NSColor.labelColor))
                out.append(NSAttributedString(string: "\n", attributes: base))
                continue
            }
            if line.hasPrefix(">") {
                let body = String(line.dropFirst(min(2, line.count)))
                out.append(NSAttributedString(string: "▎ ", attributes: base))
                out.append(inline(body, font: bodyFont, color: NSColor.secondaryLabelColor))
                out.append(NSAttributedString(string: "\n", attributes: base))
                continue
            }
            if let bullet = bulletPrefix(line) {
                out.append(NSAttributedString(string: bullet.marker + " ", attributes: base))
                out.append(inline(bullet.body, font: bodyFont, color: NSColor.labelColor))
                out.append(NSAttributedString(string: "\n", attributes: base))
                continue
            }
            out.append(inline(line, font: bodyFont, color: NSColor.labelColor))
            out.append(NSAttributedString(string: "\n", attributes: base))
        }
        if inCode, !codeLines.isEmpty {
            out.append(codeBlock(codeLines))
        }
        while out.length > 0, out.string.hasSuffix("\n") {
            out.deleteCharacters(in: NSRange(location: out.length - 1, length: 1))
        }
        return out
    }

    private static func headingAttributes(_ line: String) -> (level: Int, font: NSFont)? {
        let level = line.prefix(while: { $0 == "#" }).count
        guard level >= 1, level <= 4, line.count > level,
              line[line.index(line.startIndex, offsetBy: level)] == " " else {
            return nil
        }
        let sizes: [CGFloat] = [17, 15, 13.5, 13]
        return (level, NSFont.boldSystemFont(ofSize: sizes[level - 1]))
    }

    private static func bulletPrefix(_ line: String) -> (marker: String, body: String)? {
        if line.hasPrefix("- ") || line.hasPrefix("* ") || line.hasPrefix("+ ") {
            return ("•", String(line.dropFirst(2)))
        }
        let digits = line.prefix(while: { $0.isNumber })
        if (1..<4).contains(digits.count), line.dropFirst(digits.count).hasPrefix(". ") {
            return (digits + ".", String(line.dropFirst(digits.count + 2)))
        }
        return nil
    }

    private static func codeBlock(_ lines: [String]) -> NSAttributedString {
        NSAttributedString(
            string: lines.joined(separator: "\n"),
            attributes: [
                .font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular),
                .foregroundColor: NSColor.labelColor,
            ])
    }

    /// Inline pass: `code`, **bold**, *italic*.
    private static func inline(_ text: String, font: NSFont, color: NSColor) -> NSAttributedString {
        let plain: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
        let pattern = "(`[^`]+`|\\*\\*[^*]+\\*\\*|\\*[^*]+\\*)"
        guard let regex = try? NSRegularExpression(pattern: pattern) else {
            return NSAttributedString(string: text, attributes: plain)
        }
        let out = NSMutableAttributedString()
        let nsText = text as NSString
        var cursor = 0
        for match in regex.matches(in: text, range: NSRange(location: 0, length: nsText.length)) {
            if match.range.location > cursor {
                out.append(NSAttributedString(
                    string: nsText.substring(with: NSRange(location: cursor, length: match.range.location - cursor)),
                    attributes: plain))
            }
            let token = nsText.substring(with: match.range)
            if token.hasPrefix("`") {
                out.append(NSAttributedString(string: String(token.dropFirst().dropLast()), attributes: [
                    .font: NSFont.monospacedSystemFont(ofSize: max(font.pointSize - 1, 10), weight: .regular),
                    .foregroundColor: color,
                ]))
            } else if token.hasPrefix("**") {
                out.append(NSAttributedString(string: String(token.dropFirst(2).dropLast(2)), attributes: [
                    .font: NSFont.boldSystemFont(ofSize: font.pointSize),
                    .foregroundColor: color,
                ]))
            } else {
                let italic = NSFont(
                    descriptor: font.fontDescriptor.withSymbolicTraits(.italic),
                    size: font.pointSize)
                out.append(NSAttributedString(string: String(token.dropFirst().dropLast()), attributes: [
                    .font: italic,
                    .foregroundColor: color,
                ]))
            }
            cursor = match.range.location + match.range.length
        }
        if cursor < nsText.length {
            out.append(NSAttributedString(string: nsText.substring(from: cursor), attributes: plain))
        }
        return out
    }
}

// MARK: - stream card view

struct StreamCardView: View {
    @EnvironmentObject private var model: ReaderModel
    @ObservedObject var card: StreamCard
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if !card.title.isEmpty {
                header
            }
            sourceSection
            bodyContent
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.border))
    }

    private var header: some View {
        HStack(spacing: 6) {
            Text(card.title)
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(Theme.text)
                .lineLimit(1)
            if !card.meta.isEmpty {
                Text(card.meta)
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textFaint)
                    .lineLimit(1)
            }
            Spacer(minLength: 4)
            if card.phase == .streaming {
                cardButton(L10n.t("ui.card.stop")) { model.stopStream(card) }
                    .foregroundStyle(Theme.accent)
            }
            cardButton(copied ? L10n.t("ui.card.copied") : L10n.t("ui.card.copy")) {
                let text = card.copyText
                guard !text.isEmpty else { return }
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(text, forType: .string)
                copied = true
                Task {
                    try? await Task.sleep(nanoseconds: 1_200_000_000)
                    copied = false
                }
            }
            .opacity(card.copyText.isEmpty ? 0.4 : 1)
            cardButton("✕") { model.removeCard(card) }
        }
    }

    @ViewBuilder
    private var sourceSection: some View {
        if case .notice = card.phase {
            EmptyView()
        } else if !card.sourceText.isEmpty {
            if card.sourceJump != nil {
                citeChip
            } else {
                sourceExcerpt
            }
        }
    }

    private var sourceExcerpt: some View {
        Text(card.sourceText)
            .font(.system(size: 11))
            .foregroundStyle(Theme.textDim)
            .lineLimit(3)
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 5).fill(Theme.panelAlt))
    }

    /// AiCiteChip（design/prototype/ai.html .cite）：摘句 + 位置 ↗，
    /// 点击回跳发送时锁定的原文位置。
    private var citeChip: some View {
        Button {
            card.sourceJump?()
        } label: {
            HStack(alignment: .top, spacing: 7) {
                Text(card.sourceText)
                    .font(.system(size: 11.5))
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(2)
                    .multilineTextAlignment(.leading)
                Spacer(minLength: 6)
                HStack(spacing: 2) {
                    Text(card.sourceLoc)
                        .font(.system(size: 11.5, weight: .semibold))
                        .foregroundStyle(Theme.accent)
                        .lineLimit(1)
                    Image(systemName: "arrow.up.right")
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundStyle(Theme.accent)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.panelAlt))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Theme.border, lineWidth: 1))
            .overlay(alignment: .leading) {
                // Prototype .cite's 2px accent left edge.
                Rectangle()
                    .fill(Theme.accent)
                    .frame(width: 2)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(verbatim: "\(card.sourceLoc)"))
    }

    @ViewBuilder
    private var bodyContent: some View {
        switch card.phase {
        case .notice(let message):
            Text(message)
                .font(.system(size: 12))
                .foregroundStyle(Theme.textDim)
                .italic()
        case .error(let message):
            Text(message)
                .font(.system(size: 12))
                .foregroundStyle(Theme.danger)
                .textSelection(.enabled)
        case .empty(let stopped):
            Text(stopped ? L10n.t("backend.stream.stopped") : L10n.t("backend.stream.no-content"))
                .font(.system(size: 12))
                .foregroundStyle(Theme.textFaint)
                .italic()
        case .streaming:
            streamingBody
        case .done:
            doneBody
        }
    }

    @ViewBuilder
    private var streamingBody: some View {
        if !card.content.isEmpty {
            markdownBody(card.content)
        } else if !card.reasoning.isEmpty {
            HStack(spacing: 6) {
                ProgressView()
                    .controlSize(.small)
                Text(L10n.t("backend.stream.thinking"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textFaint)
            }
        }
    }

    @ViewBuilder
    private var doneBody: some View {
        if !card.content.isEmpty {
            markdownBody(card.content)
        } else if !card.reasoning.isEmpty {
            // Provider put everything into the reasoning field — show it dimmed.
            markdownBody(card.reasoning)
                .opacity(0.55)
        }
    }

    private func markdownBody(_ text: String) -> some View {
        Text(MarkdownLite.attributed(text))
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func cardButton(_ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(.system(size: 11))
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
        }
        .buttonStyle(.plain)
        .foregroundStyle(Theme.textDim)
        .background(RoundedRectangle(cornerRadius: 4).fill(Theme.panelAlt))
    }
}
