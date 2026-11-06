import SwiftUI
import AppKit

// Booklight's visual system. The product is paper-first: the reading canvas
// is quiet and neutral, while navigation uses the platform's own chrome and
// materials. Brand terracotta is reserved for the wordmark, the primary
// action and selected navigation — never for decoration.
//
// Every token below is semantic and appearance-aware. Hosts on Windows and
// Linux mirror the same scale (4/8/12/16/24/32) and the same restraint: one
// accent, native surfaces, typography by role rather than by pixel size.

enum Theme {
    /// Reading canvas. Light mode is warm paper; dark mode is the same warm
    /// black used by the EPUB page so the page edge never flashes.
    static let bg = Color.dynamic(light: 0xF7F4EF, dark: 0x171412)

    /// Navigation surface behind the sidebar. Distinct from `bg` so the
    /// document and its navigation read as two planes, not one flat sheet.
    static let sidebar = Color.dynamic(light: 0xEFEBE4, dark: 0x211D1A)

    /// Floating controls and grouped rows.
    static let panel = Color.dynamic(light: 0xFFFDF9, dark: 0x2A2521)
    static let panelAlt = Color.dynamic(light: 0xF2EEE7, dark: 0x322C27)

    static let border = Color(nsColor: .separatorColor).opacity(0.7)
    static let borderStrong = Color.primary.opacity(0.14)

    static let text = Color.primary
    static let textDim = Color(nsColor: .secondaryLabelColor)
    static let textFaint = Color(nsColor: .tertiaryLabelColor)

    /// The single brand accent. A touch warmer than the system orange so it
    /// belongs to paper, but it still clears 4.5:1 on both surfaces.
    static let accent = Color.dynamic(light: 0xC4623E, dark: 0xE8926A)
    static let accentHover = Color.dynamic(light: 0xAD5130, dark: 0xF0A07C)
    static let accentSoft = accent.opacity(0.12)
    static let accentBorder = accent.opacity(0.26)

    static let danger = Color(nsColor: .systemRed)
    static let success = Color(nsColor: .systemGreen)

    // Spacing scale. Use these values instead of ad-hoc numbers.
    static let s1: CGFloat = 4
    static let s2: CGFloat = 8
    static let s3: CGFloat = 12
    static let s4: CGFloat = 16
    static let s5: CGFloat = 24
    static let s6: CGFloat = 32

    enum Radius {
        static let small: CGFloat = 6
        static let medium: CGFloat = 10
        static let large: CGFloat = 14
    }

    /// Annotation chips (v1 .note-dot colors).
    static func annotationColor(_ name: String) -> NSColor {
        switch name {
        case "green": return NSColor(hex: 0x4CC383, alpha: 0.85)
        case "blue": return NSColor(hex: 0x569CFF, alpha: 0.85)
        default: return NSColor(hex: 0xFFD500, alpha: 0.9)
        }
    }

    static func annotationSwiftUIColor(_ name: String) -> Color {
        Color(nsColor: annotationColor(name))
    }

    /// Two-point rule used by quiet section headers.
    static func hairline(_ colorScheme: ColorScheme) -> Color {
        colorScheme == .dark ? Color.white.opacity(0.08) : Color.black.opacity(0.07)
    }
}

extension Color {
    static func dynamic(light: UInt32, dark: UInt32) -> Color {
        Color(nsColor: NSColor(name: nil) { appearance in
            let isDark = appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            return NSColor(hex: isDark ? dark : light)
        })
    }
}

extension NSColor {
    convenience init(hex: UInt32, alpha: CGFloat = 1) {
        self.init(
            red: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255,
            alpha: alpha)
    }
}
