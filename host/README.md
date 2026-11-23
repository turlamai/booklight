# Glaze carrier + product web UI (#75 S1–S2)

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
  (`index.html?e2e=1`, 13 steps); verdicts cover both.

## Run

```bash
racket host/main.rkt        # manual dev window (token-gated)
racket host/verify.rkt      # real-window evidence, exit 1 on fail
bash host/linux-verify.sh   # headless Linux (Xvfb + /run/user bus)
python scripts/check-rpc-surface.py   # route surface vs rivet-schema.json
```

Evidence lands in `.glaze/verify/` (boot + S1 + S2 post-journey PNGs and
`booklight-host-report.json` shaped `{s1:…, s2:…}`), collected over the
loopback HTTP adapter — no host accessibility dependency.