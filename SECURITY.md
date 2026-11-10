# Security Policy

Booklight is file-first: it opens your PDF and EPUB documents where they
already live and never imports their bytes into an app-owned library.
Reading data stays in plain, small files — `settings.json` (with the
`api_key` field always empty), annotation sidecars next to the documents
or under the config directory — and your provider API key goes into the
OS credential store, never into `settings.json`. Nothing is telemetry;
reports are taken seriously and handled quickly.

## Reporting a vulnerability

Email **security@jrtx.site** (or open a GitHub security advisory on this
repository). Include reproduction steps, affected versions, and your
assessment of impact. You will get an acknowledgment within 72 hours.

Please do not open public issues for security problems.

## Scope

In scope:

- Update trust: verification of the Ed25519-signed `update-manifest.json`
  and its artifacts, key handling and rotation
  (`app/updater.rkt`, `app/version.rkt` — embedded public key and key id
  `booklight-2026-10`, `scripts/make-update-manifest.sh`,
  `scripts/update-keys.sh`, `scripts/check-update-signing-key.py`), and
  the bounded transfer (800 MiB cap, stall and total limits)
- Document parsing and rendering: everything Booklight concludes from an
  untrusted PDF or EPUB (`racket/booklight/pdfdoc.rkt`,
  `racket/booklight/epub.rkt`; the chapter HTML sanitization that strips
  scripts and styles before the host renders it). A malformed document
  must fail with an honest diagnostic, never write outside the data
  contract
- Reading data: the per-user config directory (`settings.json`,
  the fallback `api-key` file, `annotations/` including recovery drafts)
  and the per-document annotation sidecars (`<document>.booklight.json`)
  — contract: `shared/spec/DATA-FORMAT.md`
- Provider credentials (BYOK): the secure store
  (`racket/booklight/secure-store.rkt`) — the key must stay in the OS
  credential store (macOS Keychain / Linux Secret Service / Windows
  DPAPI), the mode-0600 fallback file is only for machines without a
  desktop credential store, and no secret may be serialized into
  `settings.json` or logs
- Provider traffic: prompts and streamed completions
  (`racket/booklight/llm.rkt`, `racket/booklight/sse.rkt`) go only to the
  user-configured provider base URL; document text must not be sent
  anywhere the user did not configure
- The host/backend boundary: hosts render and install; the Racket backend
  (`app/backend.rkt`) decides. Crypto or policy sneaking into a native
  host is a bug.

Out of scope: the security of the LLM providers the user configures
(their APIs, rate limits and handling of prompt content belong to those
services), social engineering, physical access attacks, and defects in
the OS-provided credential stores (Keychain, Secret Service, DPAPI),
PDFium or WebKitGTK themselves — those belong to their vendors.

## Design notes reviewers should know

- The backend owns no listening network port. Host ↔ backend RVT1 traffic
  runs over inherited file descriptors (`serve-fds`). Outbound network is
  limited to update checks and downloads against GitHub Releases, plus
  LLM requests to the user-configured provider endpoints.
- **Hosts never implement crypto.** The Ed25519 manifest verify and the
  SHA-256 artifact verify live in the Racket backend (Rivet's
  distribution layer); native hosts poll `update-state` and render
  progress (`shared/spec/UPDATE.md`). Key rotation ships a build that
  trusts the next key before releases stop being signed with the old one.
- **The signed update feed does not imply a publisher-signed installer.**
  The macOS DMG is ad-hoc signed (not notarized) and the MSI, deb, rpm,
  AppImage and tar.gz artifacts carry no publisher code signature.
  Releases publish a checked `SHA256SUMS` and Sigstore build provenance.
  Do not represent the feed signature as an independently trusted
  publisher identity.
- Storage is plain JSON, written atomically with mode 0600 into a 0700
  config directory; document bytes are never copied. Protection
  ultimately depends on OS user permissions and filesystem protection.
  The legacy plaintext `api_key` value is migrated to the credential
  store on the next settings save; the historical `library.json` /
  `covers/` data is left untouched.
- The EPUB renderer receives sanitized chapter HTML (scripts and styles
  stripped, in-book images inlined as `data:` URLs, dead images dropped)
  and must keep treating it as untrusted content.

## Supported versions

Security fixes land on `main` first and ship with the next tagged release.
During the 0.x preview there is no LTS branch.
