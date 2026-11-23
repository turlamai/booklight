# Booklight

> Open a file. Read comfortably. Edit deliberately.

**English** · [简体中文](README.zh-CN.md)

[Releases](https://github.com/turlamai/booklight/releases) · [CI](https://github.com/turlamai/booklight/actions/workflows/ci.yml) · [License](LICENSE)

Booklight is a PDF / EPUB reader and light PDF editor for macOS, Windows and Linux. It is **not** an iBooks-style library manager. Your files stay in their original folders: there is no managed book catalog, cover library, reading-time tracking, goal, streak or database.

## Start with a file

Open PDF / EPUB through File > Open, drag-and-drop, or the system file association. An empty window shows a simple Open action. Recent paths optionally help you return to the last page; they are not a separate book library.

- **Read PDFs**: continuous / two-page view, zoom, thumbnails, outline, text search, print and bookmarks.
- **Read EPUBs**: reflowed chapters, typography controls, table of contents, search and notes.
- **Annotate**: highlights, notes and bookmarks with explicit save and failure/recovery behavior.
- **Edit PDFs lightly**: rotate, remove, insert, extract or merge pages, fill forms and export; no silent replacement of the original.
- **Optional AI**: translate, summarize or chat about selected document content with your own provider. AI requests may send that content to the chosen provider.
- **Research across documents**: search open and recent files locally, jump to the source passage and pin traceable citations to a deck.

## Files remain yours

There is no book import step. New profiles prefer annotation sidecars next to the source; existing profiles keep their earlier annotation setting. Booklight still saves small preferences and recent paths in `settings.json`, and keeps recovery drafts if annotation writes fail. Existing old `library.json` and covers are left intact for manual backup, but no longer created or used as a collection store.

[Data and recovery contract](shared/spec/DATA-FORMAT.md) · [English user guide](docs/USER-GUIDE.md) · [中文使用指南](docs/USER-GUIDE.zh-CN.md)

## Architecture

One web UI (`host/web/`) on the [glaze](https://github.com/turlamai/glaze)
carrier (system WebView + embedded Racket CS runtime) serves macOS, Windows
and Linux alike. PDF rendering uses pdf.js; page surgery stays in the
self-contained Racket reader (`racket/booklight/pdfdoc.rkt`).

One Racket core communicates through typed RPC (RVT1). The RPC surface is
the single contract (`glaze-schema.json`): the legacy shelf endpoints are
gone and the carrier speaks the current schema.

```bash
raco glaze schema check glaze-schema.json --json
raco test racket/
raco glaze dev
racket host/verify.rkt
```

[Product decision and acceptance plan](docs/FILE-FIRST-DESIGN.md) · [Developer guide](AGENTS.md)

## Install

Download from [Releases](https://github.com/turlamai/booklight/releases/latest).
Assets follow `booklight-<version>-<os>-<arch>.<ext>`:

| Platform | Installers | Portable zip (in-app updates) |
|---|---|---|
| macOS 14+ (Apple silicon / Intel) | `booklight-<version>-macos-arm64.dmg` · `-x64.dmg` | `booklight-<version>-macos-arm64.zip` · `-x64.zip` |
| Windows 10+ x64 | `booklight-<version>-windows-x64.msi` | `booklight-<version>-windows-x64.zip` |
| Linux x64 / arm64 | `booklight-<version>-linux-<arch>.deb` · `.rpm` · `.AppImage` · `.flatpak` (app id `site.jrtx.booklight`; install with `flatpak --user install ./booklight-<version>-linux-<arch>.flatpak`) | `booklight-<version>-linux-<arch>.tar.gz` |

Every release carries a checked `SHA256SUMS`, and the in-app updater
consumes the Ed25519-signed `update-manifest.json` feed (key id
`booklight-2026-11`; Sigstore provenance: `gh attestation verify <file>
-R turlamai/booklight`).

macOS release builds are Developer ID-signed and notarized by the release
pipeline (a tag release fails closed without signing credentials). Local
development builds are ad-hoc signed: right-click the app and choose Open
on first launch to clear the Gatekeeper prompt.

## License

[AGPL-3.0](LICENSE)
