# File-first Booklight — 2026-10-29 product decision

Booklight is a **document reader and light editor**, not a book collection manager. This decision supersedes earlier iBooks-inspired shelf, goals, catalog and research-workbench-first plans.

## Primary workflow

1. Open an existing PDF or EPUB with File > Open, system file association or drag-and-drop.
2. Read with native navigation, search, page/chapter controls, accessible keyboard actions, themes and optional AI.
3. Annotate with explicit saved/pending/failed states. New profiles prefer the annotation sidecar next to the existing source file; old central settings remain readable. A failed save must retain a recoverable draft.
4. Edit PDF pages or fill forms in a working view and explicitly export/save. Never silently replace or clone the source.
5. Close and return to a small welcome view. Recent paths and the last page are optional shortcuts, **not an import/catalog**.

## Deliberately not in the product

- Library management: `library.json`, cover caches, collections, reading/want/finished categories and missing-book relinking.
- Reading habit accounting: seconds, daily goals, streaks, achievements and dashboards.
- Automatic copy of legacy app data, document duplication, mandatory cloud storage or a local database.
- Forced monetization, research-workbench sprawl or AI required for basic reading.

## What may be written

- Source PDF/EPUB: read in its original folder; only explicit editing/export operations may produce a separate output.
- `settings.json`: small app preferences, provider settings and at most 12 recent file paths/pages. No reading durations.
- `<document>.booklight.json` sidecar, by default for *new* profiles when the user creates an annotation; existing profiles' selected central store remains supported.
- Annotation recovery journal only when staging a write; remove upon successful save. Never report a pending recovery as successfully saved.
- Signed updater cache as already required by the updater; do not mistake an app update for a document copy.

No automatic deletion of historical `library.json` or `covers/`: leave existing user data untouched for explicit backup or migration.

## Implementation strategy

1. Make all native hosts launch into a small Open interface and directly open each selected/dropped path, without importing.
2. Remove library persistence and reading-time services; retain wire-compatible no-write RPCs only until native generated clients can be migrated in one coordinated schema change.
3. Delete dead shelf UI, regenerated clients and tests once native builds and cross-platform package acceptance pass.
4. Protect editing and annotations: check read-only folders, failures, atomic saving and recovery. Clarify explicit Replace Original versus Export semantics.
5. Rework public docs, screenshots, i18n and packaging to describe the product actually shipped.

## Acceptance gates

- On a fresh profile: opening PDF/EPUB creates no `library.json`, `covers/`, source-document copy or reading stats ledger.
- macOS/Windows/Linux: open and drag paths, reopen recent files, multi-document UX, close, search, save/export, annotations and failure recovery.
- Existing central annotations and PDF/EPUB remain readable; no silent format or path migrations.
- Validate full Racket tests, Rivet schema and generated client checks, packaged native accessibility journeys, and installer behavior. **PR status or source edits do not prove native runtime correctness.**
