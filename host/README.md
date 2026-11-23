# Glaze carrier (#75 S1)

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
  verify. The product web UI (library/reader chrome/tabs, EPUB surface)
  lands in S2; the probe page is harness-only, never product UI.

## Run

```bash
racket host/main.rkt        # manual dev window (token-gated)
racket host/verify.rkt      # real-window evidence, exit 1 on fail
bash host/linux-verify.sh   # headless Linux (Xvfb + /run/user bus)
python scripts/check-rpc-surface.py   # route surface vs rivet-schema.json
```

Evidence lands in `.glaze/verify/` (boot + post-journey PNGs and
`booklight-host-report.json`), collected over the loopback HTTP adapter —
no host accessibility dependency.