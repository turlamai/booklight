# Changelog

All notable changes to Booklight are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is semver.

## [Unreleased]

### Changed

- Windows and Linux now use the same welcome, reader, sidebar, annotation and
  settings information architecture as macOS while keeping their native
  WinUI/Fluent and GTK/Adwaita chrome. Windows moves Forms into the left
  sidebar, adopts a four-mode icon switcher, a two-column welcome and a
  grouped command row; Linux adopts the two-column welcome with a product
  mark and recent-file rows. Terms, ordering and shortcut semantics stay
  shared across the three hosts.
- Settings is now an app-level surface on all three hosts: the macOS Settings
  scene, the Windows gear-opened dialog and the Linux gear-opened window. The
  AI inspector keeps only Translate / Summarize / Chat, so the entry and the
  navigation model match instead of hiding Settings in a fourth AI tab.

## [0.9.0] - 2026-11-04

### Added

- File-first product model: Booklight opens PDF and EPUB files in place
  from File > Open, drag-and-drop or the system file association, and
  remembers recent paths. The managed book catalog, cover library,
  reading-time tracking, goals and streaks are gone; existing
  `library.json` files are left untouched on disk and are no longer read
  or created.
- AI send review on macOS, Windows and Linux: before a translation,
  summary or chat request starts, a native dialog shows the exact action,
  scope (selection/page/chapter/document) and the precise Unicode
  character counts that will leave the machine; cancelling preserves the
  typed question and conversation, and no provider is contacted for the
  preview.
- Unsaved-PDF close guard on all three platforms: closing a window, tab
  or the application with in-memory page edits raises a
  Save As / Discard / Cancel dialog; Cancel keeps the document open and
  dirty, Save As writes only to an explicitly chosen destination, and the
  source file is never silently replaced. The Discard button carries the
  save-panel-convention Command+D shortcut on macOS.
- Multiple EPUB documents on Windows: each EPUB opens in its own native
  tab with independent chapter state and annotations; switching and
  closing tabs never replaces another book or leaks state across
  documents.
- Rotation shortcut: Tools > Rotate Page answers Command+R on macOS and
  Linux.
- Branded title-bar icon on Windows: the packaged executable's group
  icon is surfaced in the WinUI title bar instead of the generic
  placeholder.
- Recoverable annotation persistence on macOS, Windows and Linux. Booklight
  now stages an atomic app-owned recovery copy before replacing the selected
  central or sidecar store, restores a newer pending copy on reopen, and
  exposes saving, recovered and failed states in the native Notes pane with
  explicit retry and export actions.

### Changed

- Annotation save failures are no longer treated as success, and application
  shutdown flushes live PDF and EPUB annotation documents before the backend
  stops. Sidecars use the documented `.booklight.json` name while legacy
  `.pdfgist.json` files remain readable.

### Removed

- The Books-style home: shelf, catalog import, covers, reading-time
  ledger and goals are removed from all three hosts together with the
  library RPC surface behind the scenes. Recent original paths replace
  the bookshelf as the way back to a document.

## [0.8.0] - 2026-10-22

### Added

- **The outline follows you.** The outline's active entry — the deepest
  item at or before the current page, or the current chapter for EPUB —
  is highlighted and gently scrolled into view as the reading position
  changes, on all three platforms.
- **Sepia.** A third page surface (deep cream parchment) between paper
  and night; existing palettes untouched.
- **Night inverts PDF page content on macOS.** Windows and Linux already
  ran a PDF.js-style negative filter inside their PDFium pipelines; the
  macOS leg uses a custom PDFPage drawing a difference-blend pass, with
  the document re-opened in place (position, zoom, annotations kept).
- **EPUB annotations.** Selections anchor by chapter + character offsets
  (backward-compatible optional `anchor` in the annotations format —
  PDF files stay byte-identical), render as multi-segment marks in the
  page, and surface in the notes sidebar with the annotate-selection
  row (three highlight colors, underline, strike), one-step undo and
  the Markdown digest export.

### Documented boundaries

- The UI language switch keeps its relaunch-to-apply contract: a live
  switch would mean rebuilding both the WinUI and GTK window trees;
  the reasoning is recorded in docs/UX-BENCHMARKS.md.

## [0.7.0] - 2026-10-22

### Added

- **One-step undo for annotation deletes.** Deleting an annotation now
  shows a five-second 撤销 banner on every platform (taskly-benchmark
  interaction). Deletion still persists immediately; undo re-inserts the
  annotation at its old position and persists again. A new delete
  re-stages the window, switching documents dismisses it.

## [0.6.0] - 2026-10-22

### Added

- **Annotation export.** The notes pane gains 导出批注…: the domain core
  renders your highlights, underlines, strikes and notes — plus a
  bookmarks section — as a Markdown digest you keep beside the book.
  Pages ascend, content stays verbatim, and what lands in the file is
  exactly what the pane shows (unsaved edits included). Labels follow
  the interface language.

## [0.5.0] - 2026-10-22

### Added

- **Reading themes on every desktop.** The reader page surface is now a
  setting: paper / night, independent of the app chrome, with "system"
  following the app appearance. EPUB pages re-theme live and the PDF page
  canvas follows; the EPUB font scale (A+/A-/reset) persists through the
  settings store and every reopened book starts where you left the type.
- **Collection management completes itself.** Collections can now be
  deleted on all three platforms (macOS context menu, Windows context
  flyout, Linux right-click popover — one confirmation, books stay), and
  empty collections are finally visible: a new `library-collections` RPC
  feeds each sidebar the shelf's own stored list.
- **Reading Now.** A first-class smart view (opened but not finished) in
  the shelf sidebar on all three platforms.
- **Read aloud is configurable.** TTS gains a voice picker and speaking
  rate (0.5x–3.0x), persisted per host; Linux maps the rate onto
  speech-dispatcher.
- **UI language override on Windows and Linux.** The mac host's in-app
  中文/English switch reaches the other two hosts (applies on relaunch) —
  zh readers on an English system are no longer stuck.
- **Keyboard paging everywhere.** Windows catches up with PageUp/PageDown
  page flips; the EPUB zoom reset now exists on all three platforms.

### Changed

- The release pipeline refuses to publish without a signed update
  manifest (a warning became a hard gate, taskly-parity).
- The update contract is documented (shared/spec/UPDATE.md), and
  docs/UX-BENCHMARKS.md starts the booklight design-benchmark ledger.

## [0.4.0] - 2026-10-20

### Added

- **Complete EPUB shelf metadata.** EPUB imports now extract both cover art
  and `dc:creator` author metadata in the shared Racket domain core, so the
  macOS, Windows and Linux shelves stay consistent without host-side parsing.
- **Packaged product acceptance on every desktop platform.** CI now opens the
  sample EPUB through the real RVT1 backend in packaged macOS, Windows and
  Linux apps and verifies the parsed title and table of contents through each
  native accessibility tree.

### Changed

- Rivet is pinned to v0.12.0 for the build, packaging and acceptance toolchain.

### Fixed

- Linux defers startup file-open requests until the backend is ready.
- EPUB titles and outlines now propagate reliably through all three native
  hosts, including explicit WinUI reader-tab tracking and GTK sidebar timing.

## [0.3.0] - 2026-10-17

### Added

- **The reading timer returns — live.** The Books-style reading home
  ships on all three platforms: a per-second timer (mm:ss) toward a
  20-minute daily goal on a semicircular gauge, a weekday streak row
  (a day counts after a full minute), a black continue-reading pill
  that opens your last book, and a toolbar mini-ring tracking today's
  progress. Reading minutes accrue only while a reader window is
  focused, and stay device-local by design.

### Fixed

- Linux: the legacy watermark dialog sent opacity in the wrong unit
  (×10 instead of ×1000) — the same fix the newer sheet already had.

## [0.2.0] - 2026-10-17

### Added

- **The Books home layout on all three platforms** — a source-list
  sidebar (home / smart groups: want-to-read, finished, PDF, EPUB /
  my collections with creation) over a home digest (continue-reading
  and recently-added cover strips) or the cover grid for the selected
  scope. Closing the last book returns to the shelf.
- **Tools menu, all platforms**: watermark baking (text / size /
  opacity), export the current page as an image, export the document
  text as .txt — alongside page surgery and AcroForm filling.
- **Interface language setting** (follow system / 中文 / English).

### Changed

- **All-white reading chrome** — every surface uses Apple semantic
  colors; the terracotta accent is reserved for interactive elements.
  Dark mode follows the system.
- The reading timer from the early shelf build is gone — not this
  product's story.

### Fixed

- Windows: the update flow can no longer wedge the app (poller rides
  out transient RPC errors; installs only end the old process after
  the new one is confirmed running); SIGTERM works again.

## [0.1.0] - 2026-10-16

The first release of **Booklight** — the reading-first rebuild of what
briefly existed as PDFGist (its release history was retired with the old
identity; the shelf imports those books on first launch).

### Added

- **The shelf** — launch straight into your bookshelf: cover grid with
  reading progress, collections, search, and import that references your
  files in place (books are never copied; the shelf file is a single
  documented, stable `library.json` you can keep in your sync drive —
  see [DATA-FORMAT](shared/spec/DATA-FORMAT.md)).
- **Reading** — tabbed PDF (continuous scroll, two-page book view, zoom /
  fit-width, thumbnails, outline, full-text search) and EPUB (reflowable
  serif typography, chapter TOC, whole-book search) on macOS, Windows and
  Linux; continue-reading everywhere.
- **AI at your elbow** — selection translation, page / document summaries,
  and document chat, all streamed, all pointed at the provider you choose
  (GLM, DeepSeek, Kimi, Qwen, Doubao, SiliconFlow, Ollama, or any
  OpenAI-compatible endpoint). Your key stays on your machine.
- **Annotations** — three-color highlights, underline, strikeout, notes
  and bookmarks stored beside each book (sidecar JSON) or in the central
  store, ready for cloud-drive sync; byte-compatible across all three
  hosts.
- **Light editing** — a dedicated Tools menu: rotate, insert blank page,
  delete, extract, merge (in-memory; source files untouched) and AcroForm
  filling with export of the filled copy.
- **Comforts** — text-to-speech, printing, split view, password-protected
  PDFs, drag-and-drop import, interface language (follow system / 中文 /
  English), localized system chrome.
- **Signed self-updates on all three platforms** — Ed25519-verified
  manifest, SHA-256-checked download, installs only on your click.

### Local-first data

Books stay in your folders and sync drives; progress and annotations ride
beside each book in sidecars; the shelf is one documented file. No
account, no telemetry — see [DATA-FORMAT](shared/spec/DATA-FORMAT.md).
