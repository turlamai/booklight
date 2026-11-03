# Booklight UX baseline — file-first document reader

Decision: **2026-10-29-file-first**. The previous Apple Books/iBooks shelf and reading-goals benchmark is retired. The design target is a good native document reader/editor, not a book collection or habit tracker. [Product contract](FILE-FIRST-DESIGN.md).

## Benchmarks worth learning from

- **macOS Preview:** File > Open, native windows/tabs, represented file identity, zoom and page actions, clear export and dirty-state behavior. Adapt the principles on macOS rather than copying its styling everywhere.
- **SumatraPDF:** fast startup, low memory footprint, keyboard navigation and an unobtrusive reader. Keep the PDF/EPUB experience lightweight.
- **Skim and PDF annotation tools:** page navigation, search, annotation list and recoverable edits. Avoid controls that hide whether a note is saved.
- **Foliate / document readers:** readable EPUB typography, chapter navigation and progress resume, without requiring an app-managed book database.
- **Windows and Linux native conventions:** system picker/portal, standard shortcuts, accessibility semantics, logical tab/selection behavior and readable empty states.

## Product surfaces

| Situation | Expected behavior | Anti-pattern |
|---|---|---|
| No document open | Brand, primary Open action, optional recent paths | Immersive bookshelf, achievements, onboarding tour |
| PDF open | Source title, main document, optional thumbnail/outline/notes panel | Always-on crowded panels |
| EPUB open | Chapter text and TOC, search, typography controls | Catalog metadata before reading |
| User drops files | Open each original path directly | Import, copy or create a hidden cover cache |
| User annotates | Save status; retry/export on failure | Silently swallowing write errors |
| User edits PDF | Explicit save/export and confirm overwrite | Replacing the source without consent |
| Missing/renamed file | Explain missing path and allow picking again | Hiding failure behind stale book-cover tiles |
| AI action | Preview provider and exact text scope | Automatic upload or unclear origin of answers |

## File ownership

The file manager (Finder, Explorer, Nautilus, etc.) owns folder organization. Booklight does not own a shadow file collection. Recent paths and a resume page may be stored in preferences, but never infer total read time, build a streak, or create goals.

## Verification

Real packaged native UI acceptance on macOS, Windows and Linux; keyboard and accessibility tree; explicit read-only folder and failed-save cases; original-file-byte comparison after reading and annotations. Screenshots and design reviews alone do not make a feature finished.
