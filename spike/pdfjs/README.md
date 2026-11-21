# pdf.js spike (#75 S0)

Biggest technical risk of the rivet → glaze rewrite: the PDF surface has no
native home under glaze, so it moves to pdf.js inside the system webview.
This spike proves the render/selection/edit-loop on every declared platform
before any production UI slice lands.

## What it proves

1. pdf.js (legacy build, vendored, self-hosted worker/cmaps/standard_fonts)
   renders the fixture in WKWebView / WebView2 / WebKitGTK.
2. The text layer builds and native selection over it maps to highlight
   overlays positioned in page coordinates (content ink `--hl-yellow`,
   reading alpha .45 — never a UI accent).
3. Page surgery stays in the kept domain core: `api/spike/edited-pdf`
   parses the fixture with `parse-pdf`, rotates page 1 by 90° with
   `pdf-rotate-pages!`, and serves `pdf->bytes`; pdf.js re-renders those
   bytes and the harness asserts `viewport.rotation === 90`.
4. Real-window evidence: boot + post-journey PNGs and the typed report
   (`.glaze/verify/`), collected by `verify.rkt` over the loopback HTTP
   adapter — no host accessibility dependency.

## Run

```bash
racket spike/pdfjs/make-fixture.rkt   # regenerates fixture.pdf (committed)
racket spike/pdfjs/host.rkt           # manual dev window (token-gated)
racket spike/pdfjs/verify.rkt         # real-window evidence, exit 1 on fail
bash spike/pdfjs/linux-verify.sh      # headless Linux (Xvfb + /run/user bus)
```

CI: `.github/workflows/spike-pdfjs.yml` (ubuntu x64/arm64, macos, windows).

## Decisions recorded here (production slices inherit)

- pdf.js is **render-only**; `racket/booklight/pdfdoc.rkt` remains the sole
  PDF read/write authority (AGENTS.md contract). No third-party PDF lib in
  the backend.
- Legacy build chosen for webview floor compatibility; version pinned by
  the vendored directory (3.11.174, Apache-2.0, see THIRD_PARTY_NOTICES.md).
- Tokens come from `design/prototype/shared.css`; the spike adds no value
  table. Highlight overlay alpha .45 is a spike reading; the production
  PDF surface slice pins it against the EPUB highlight contract with
  pixel evidence.
- The fixture is hand-built (two pages, base-14 Helvetica) so the text
  layer exercises vendored standard_fonts; regenerated deterministically
  by `make-fixture.rkt`.