# Booklight

> Open a file. Read comfortably. Edit deliberately.

**English** · [简体中文](README.zh-CN.md)

[Releases](https://github.com/turlamai/booklight/releases) · [CI](https://github.com/turlamai/booklight/actions/workflows/ci.yml) · [License](LICENSE)

Booklight is a native PDF / EPUB reader and light PDF editor for macOS, Windows and Linux. It is **not** an iBooks-style library manager. Your files stay in their original folders: there is no managed book catalog, cover library, reading-time tracking, goal, streak or database.

## Start with a file

Open PDF / EPUB through File > Open, drag-and-drop, or the system file association. An empty window shows a simple Open action. Recent paths optionally help you return to the last page; they are not a separate book library.

- **Read PDFs**: continuous / two-page view, zoom, thumbnails, outline, text search, print and bookmarks.
- **Read EPUBs**: reflowed chapters, typography controls, table of contents, search and notes.
- **Annotate**: highlights, notes and bookmarks with explicit save and failure/recovery behavior.
- **Edit PDFs lightly**: rotate, remove, insert, extract or merge pages, fill forms and export; no silent replacement of the original.
- **Optional AI**: translate, summarize or chat about selected document content with your own provider. AI requests may send that content to the chosen provider.

## Files remain yours

There is no book import step. New profiles prefer annotation sidecars next to the source; existing profiles keep their earlier annotation setting. Booklight still saves small preferences and recent paths in `settings.json`, and keeps recovery drafts if annotation writes fail. Existing old `library.json` and covers are left intact for manual backup, but no longer created or used as a collection store.

[Data and recovery contract](shared/spec/DATA-FORMAT.md) · [English user guide](docs/USER-GUIDE.md) · [中文使用指南](docs/USER-GUIDE.zh-CN.md)

## Native architecture

| Host | Stack |
|---|---|
| macOS | SwiftUI, PDFKit |
| Windows | WinUI 3, PDFium |
| Linux | GTK4, PDFium, WebKitGTK |

One Racket core communicates through [Rivet](https://github.com/turlamai/rivet) typed RPC. The old shelf RPC endpoints remain transitional non-persistent compatibility shims; this is a **refactor in progress**, not an assertion that every platform release is already rebuilt and verified.

```bash
raco rivet doctor --json
raco rivet schema check rivet-schema.json --json
raco test racket/
raco rivet build
raco rivet acceptance --json
```

[Product decision and acceptance plan](docs/FILE-FIRST-DESIGN.md) · [Developer guide](AGENTS.md)

## License

[AGPL-3.0](LICENSE)
