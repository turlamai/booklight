# DATA-FORMAT — Booklight's file-first storage contract

**Booklight opens files where they already live.** It does not import document bytes into an app-owned library. This contract applies to the 2026-10-29 file-first refactor; see [design and migration](../../docs/FILE-FIRST-DESIGN.md).

| Data | Location | Purpose |
|---|---|---|
| PDF and EPUB source | User's existing folder | Read in place; no copy, auto-move or database |
| App preferences | `<config-dir>/settings.json` | View/theme/provider configuration and up to 12 recent file paths/pages |
| Default annotations for new profiles | `<document>.booklight.json` | Highlights, notes and bookmarks, when the first annotation is saved |
| Existing centrally configured annotations | `<config-dir>/annotations/<path-hash>.json` | Kept readable for existing users, without automatic conversion |
| Recovery draft | `<config-dir>/annotations/recovery/` | Temporary journal if the primary annotation save fails |
| Old catalog and covers | Existing `library.json` / `covers/` | Historical user data left untouched, no longer read/written by the new file-first workflow |

`settings.json` still uses the existing per-user config-directory convention. The `recent_files` section records the original path, title, last page, scroll ratio and last-open timestamp for resume. It does **not** store reading durations, goals, streaks or library membership.

## Existing PDFGist profiles (zero-copy compatibility)

If `site.jrtx.booklight` has **not** been created but an older sibling
`site.jrtx.pdfgist` profile exists, Booklight reads and writes that older
settings/central-annotation directory **in place**. No copy, move, rename or
deletion occurs. If both directories exist, the Booklight profile takes
precedence; back up both before any manual reconciliation. An explicitly
stored `annotation_sidecar: false` remains central, and a legacy settings
file without that key also remains central. New installations without a
settings file use sidecars by default.

## Annotations

Central and sidecar stores use the existing JSON form:

```json
{
  "annotations": [
    {
      "id": "a-123",
      "page": 3,
      "rects": [{ "x": 10, "y": 20, "width": 100, "height": 12 }],
      "excerpt": "Selected text",
      "color": "yellow",
      "kind": "highlight",
      "note": "",
      "created": 1792100000
    }
  ],
  "bookmarks": [{ "page": 12, "label": "Page 12", "created": 1792100000 }]
}
```

New settings default to sidecar storage, but previously saved `annotation_sidecar` settings remain authoritative. Legacy `<document>.pdfgist.json` sidecars stay readable. Sidecars are additional annotation files, **not copies of the PDF or EPUB**. If a source directory is read-only, writing a sidecar can fail; Booklight must show the unsaved state instead of silently redirecting into another store.

An annotation save stages a recovery journal before replacing the primary via atomic write. Successful saves clear the journal. Failed writes retain the draft for retry/export; reopening selects the pending recovery draft if newer than the primary. A later external change to the primary takes precedence. The original document bytes do not change when writing sidecar annotations.

## Source editing and old data

PDF page edits and form exports use an explicit output destination. Do not overwrite a source without affirmative user action and safe save semantics. Current preview builds require independent validation of each native host's save flow.

Older `library.json` is **not** a database to migrate automatically; it contains old paths, cover pointers and collections only. New startup does not delete it. Copy it manually if you need an archive, then continue working with the original PDF / EPUB paths.

See [the user guide](../../docs/USER-GUIDE.md) for recovery and privacy notes.
