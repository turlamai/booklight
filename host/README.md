# Glaze carrier + product web UI (#75 S1–S5)

The rivet → glaze re-line keeps the Racket domain core (`racket/`) and the
rivet backend (`app/backend.rkt`) untouched. This directory is the single
web UI's backend adapter: it drives the production backend over real
in-process RVT1 frames through `rivet/bridge` (extracted upstream from
Podlens: turlamai/glaze#159 → turlamai/rivet#493) and exposes every
registered RPC 1:1 as a typed glaze route.

## Decisions

- **Carrier**: production runs glaze's first-class HTTP adapter on
  127.0.0.1 with a per-launch token; `BOOKLIGHT_TRANSPORT=native` switches
  to the GLZ1 carrier for development. Same decision shape as podlens#63.
- **Single source**: `rivet-schema.json` stays the RPC contract.
  `rivet/bridge`'s schema codec converts camelCase JSON DTOs to
  field-ordered wire values and back; `scripts/check-rpc-surface.py` gates
  the route surface (names, arity, camelCase keys, scalar types) against
  the schema, and `glaze-schema.json` is the committed glaze baseline.
- **Boundary conventions**: Bytes arguments/results ride base64 strings
  (no line separators); enums (projected as record kinds in the schema)
  ride strings validated against their case list; Void/record/list ride
  the Value boundary.
- **In-process transport**: the backend serves RVT1 over a pipe pair
  inside the host process; the static require of `app/backend.rkt` keeps
  `raco exe` embedding working for packaged builds (S6).
- **S1 scope**: carrier + 34-RPC surface + harness probe page + real-window
  verify. The probe page (`probe.html`) is harness-only, never product UI.
- **S2 scope**: product web UI (`index.html` + `app.css` + `app.js`) —
  library/welcome with file-first recents, reader chrome (titlebar, document
  tabs, sidebar 目录/批注 tabs, statusbar) and the EPUB surface (toc, chapter
  prose, chapter nav, reading theme + font scale). PDF rides the S0-verified
  pdf.js render-only layer; page surgery and selection/pending-edit bars
  remain S3.

## S2 product UI contracts

- **Tokens single source**: `design/prototype/shared.css` is served by the
  carrier at `/shared.css`; `app.css` carries layout only (no token table).
  The runtime public dir is assembled at startup (`assemble-public-dir`)
  from `host/web/` + `shared.css` + the vendored `spike/pdfjs/vendor` tree,
  so no copies are committed.
- **Strings single source**: `shared/i18n/{zh,en}.json` served at
  `api/host/i18n`; zh default. No new i18n keys (i18n is Keep).
- **Native open panel**: glaze `make-dialog-routes` (`api/dialog/open`),
  granted via the `dialog:open` capability.
- **Render bytes**: `api/host/file` returns base64 bytes for pdf/epub only,
  under the `host:file` capability; the page only passes user-picked or
  recent paths. Production window runs with `host-capability`; the verifier
  runs in open-route mode (no token) so headless CI needs no grant.
- **Verify**: `host/verify.rkt` runs the S1 probe journey (`probe.html?e2e=1`,
  12 steps) then resets the report and runs the S2 product journey
  (`index.html?e2e=1`, 29 steps incl. S3+S4); verdicts cover both, plus a
  pixel-evidence stage (`index.html?e2e=1&stage=pdf`) that leaves highlight
  ink + the pending-edit bar on screen for the capture.

## S3 PDF production surface

- **Selection**: pdf.js `renderTextLayer` provides the native selection
  source; client rects convert to PDF user-space (points, bottom-left,
  unrotated — same space as the macOS PDFKit host) via
  `viewport.convertToPdfPoint`, and back via `convertToViewportPoint` for
  persisted highlight/underline/strike ink.
- **SelectionBar**: three highlight colors + underline + strike + copy;
  annotations persist through `set-annotations` / `load-annotations`
  (sidecar/central per settings), with `annotation-recovery-pending` +
  retry-save in the notes tab.
- **PendingEditBar**: rotate / delete / insert-blank / append / bake preview
  by chaining `edit-*` RPCs over carrier-managed scratch files
  (`api/host/scratch`); undo / discard restore prior / original bytes;
  save-as uses the native save panel + atomic `api/host/write`. extract
  exports via save-as. The source file is never overwritten implicitly.
- **Capabilities**: `dialog:open`, `dialog:save`, `host:file`, `host:write`.

## Run

```bash
racket host/main.rkt        # manual dev window (token-gated)
racket host/verify.rkt      # real-window evidence, exit 1 on fail
bash host/linux-verify.sh   # headless Linux (Xvfb + /run/user bus)
python scripts/check-rpc-surface.py   # route surface vs rivet-schema.json
```

Evidence lands in `.glaze/verify/` (boot + S1 + S2 post-journey PNGs and
`booklight-host-report.json` shaped `{s1:…, s2:…, s3:…, s4:…, s5:…, drop:…}`), collected over the
loopback HTTP adapter — no host accessibility dependency.
## S4 settings / research / AI inspector

- **Settings**: native-grouped form over `get-settings` / `save-settings` /
  `save-reading-prefs` / `set-locale` / `save-api-key` / `test-connection` /
  `list-presets`; presets fill base URL + default model; connection test
  surfaces suggested-model chips; API key never round-trips to the page.
- **Research**: searches open documents in place (EPUB per chapter via
  `epub-chapter-text`, PDF per page via pdf.js `getTextContent`); hit cards
  carry mark + source pill, jump-to-source, copy-citation
  (`ui.research.cite-format`) and pin-to-deck; the deck persists as `cite`
  annotations through the kept annotation store and jumps back.
- **AI inspector**: chat / summarize / translate tabs with selection / page /
  document scope; every send goes through `preview-ai-send` and the review
  sheet (`ui.ai-preview.body`); only 「继续」 issues `chat` / `summarize-text` /
  `translate-text`; answers stream over the carrier SSE bus
  (`/glaze/events`: stream-chunk / stream-done / stream-error) with
  `stop-stream`; history pairs feed subsequent previews.
- **Verify**: +8 S4 journey steps (29 total) and a research pixel-evidence
  stage (`index.html?e2e=1&stage=s4`) leaving hit cards + citation deck on
  screen for the capture.

## S5 reading-quality pixel evidence

- Stage `index.html?e2e=1&stage=s5` resets reading prefs to paper/1000, then
  asserts at runtime: highlight ink == `--hl-yellow` at opacity .45 (PDF);
  measure max-width 608px + padding-top 40px; prose family Georgia/Source Han
  Serif stack, line-height/font-size == 1.9, text-align justify; paper / night
  / sepia reader background+ink equal the `shared.css` reading tokens; font
  scale 1200 permille == 20.4px computed then restored to 1000.
- Also asserts citeblock 15px/1.8 + 3px rule @ .88, underline ink == `--accent`,
  per-line selection-to-ink IoU >= 0.8, stable 608px measure across chapters
  with no horizontal overflow, and smooth scroll feel.
- Ends on sepia EPUB prose; verify captures `booklight-host-s5.png` and gates
  fourteen s5 verdicts plus the capture.

## Drop mask pixel evidence

- Stage `index.html?e2e=1&stage=drop` shows the drag-over mask over the
  library, proves a synthetic drop of the EPUB fixture opens it (bytes staged
  through `api/host/scratch` + `api/host/write` under the dropped file name),
  then leaves the mask on screen; verify captures `booklight-host-drop.png`.
