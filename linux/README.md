# Linux host (GTK4)

The Linux host is a first-party GTK4 shell over the same embedded Racket CS
backend the macOS and Windows hosts drive: RVT1 RPC through the generated
client (`GeneratedBackend.hpp`, do not edit), every completion and event
dispatched back to the main loop before widgets are touched.

State: **feature parity with the macOS reading host (P2)**: reading core, AI
workflow, settings, EPUB, page editing, online updates, About, plus the P2
surface — password prompt for encrypted PDFs, annotation
highlights/underline/strike with the notes pane, bookmarks, printing and
AcroForm filling with export.

## What is here

- `src/pdfium.{hpp,cpp}` — PDFium binding. The library is dlopen'd (not
  linked) so a missing `libpdfium.so` surfaces as a readable message
  instead of a loader abort; the .so is searched beside the executable,
  then `$RIVET_PDFIUM_PATH`, then the normal loader search. Pages render
  into cairo ARGB32 surfaces (PDFium's BGRA is byte-identical on
  little-endian), text/outline come from the same C API. The AcroForm
  surface (form fill environment, field read/write, `FPDF_SaveAsCopy`) is
  bound from the pinned archive's own headers for type layout but still
  dlsym'd — a libpdfium without it loses only forms, never rendering.
- `src/annotations.{hpp,cpp}` — the annotation storage schema (sidecar /
  central JSON, byte-compatible with the mac/win hosts; rects in PDF page
  space, bottom-left origin, y-up).
- `src/pdfview.{hpp,cpp}` — reading core: PdfDocument (sizes + text-page
  caches + form surface), PdfTab (continuous scroll, single/double spread,
  zoom + fit-width, selection, find, thumbnails, outline, annotation
  overlay). All PDFium calls run on the UI thread.
- `src/printing.{hpp,cpp}` — GtkPrintOperation with a draw-page callback
  over the document's cairo render path (scale-down-to-fit, printer DPI).
- `src/forms.{hpp,cpp}` — AcroForm pane: enumerate text/check/choice
  fields, fill in memory (FORM_* path with a verified annotation-dictionary
  fallback), export through a save dialog via `FPDF_SaveAsCopy`. Values are
  never persisted.
- `src/panels.{hpp,cpp}` — AI panel: translate / summarize / chat stream
  cards (v1 StreamBuffer semantics: reasoning dimmed until the first
  answer delta) and the settings tab (presets with per-preset stash, API
  key, connection test, target language, view mode, sidecar, appearance).
- `src/epub.{hpp,cpp}` — EPUB over WebKitGTK (webkitgtk-6.0): the whole
  book as one continuous scroll, TOC popover, font scaling, whole-book
  search with in-page `<mark>` highlights, selection bridge to the AI
  actions. Builds without WebKitGTK (`-DPDFGIST_ENABLE_EPUB=OFF`) with a
  readable fallback.
- `src/main.cpp` — window assembly: header bar (open / page-edit menu /
  bookmark / print / TOC / primary menu), notebook tabs (the
  EPUB tab stays last), left sidebar (thumbnails + outline + notes +
  forms), find bar, status bar, keyboard navigation, drag & drop,
  page-level editing dialogs (delete / rotate / insert blank / extract /
  merge / bake text), the password prompt with wrong-password retry, the
  three-shape update flow (deb/rpm → package manager hint, AppImage →
  in-place swap + restart consent, tarball → extraction hint) and the About
  dialog.
- `src/host.{hpp,cpp}` — dispatch-to-main-loop helper, install-shape
  detection, host-side prefs (theme, panel visibility) in
  `<config-dir>/site.jrtx.pdfgist/host.conf`.
- `cmake/StagePdfium.cmake` — copies `libpdfium.so` into the rivet stage
  (`.rivet/stage`) when one exists so packages and AppImages bundle it.

## PDFium binaries

Fetched at configure time from bblanchon/pdfium-binaries with a pinned tag
and sha256 (see `CMakeLists.txt`); binaries never enter git. Bumping the
pin means moving the tag and **both** architecture digests together.

## Building by hand

Requirements: embeddable Racket CS (static `libracketcs` + boot files),
CMake ≥ 3.24, Ninja or Make, pkg-config, GTK 4 (≥ 4.10), WebKitGTK 6.0,
json-glib, cairo, zlib, LZ4, curses. The standard prebuilt Linux Racket
installer does not ship `libracketcs`; build Racket CS from source per
Racket's embedding guide (or use rivet's `setup-embed-racket` action).

```bash
export RIVET_ROOT=/path/to/rivet
export RIVET_RACKET_INCLUDE=/path/to/racket/include
export RIVET_RACKET_LIBRARY=/path/to/racket/lib/libracketcs.a
cmake -S linux -B /tmp/pdfgist-linux-build
cmake --build /tmp/pdfgist-linux-build
```

The executable must sit beside a staged runtime to start: `runtime/*.boot`
and `res/core.zo` — `raco rivet build` (and `package`/`release`) create
that layout automatically and are the supported path.
