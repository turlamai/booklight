# Contributing to Booklight

Thank you for looking under the hood. Booklight is free software under
[AGPL-3.0](LICENSE): by submitting a pull request you agree your
contribution is licensed under AGPL-3.0, and you confirm you have the
right to submit it.

## Ground rules

1. **The wire contract is cross-platform.** `app/backend.rkt` declares the
   typed RPC/Event/State surface. Changing any of it is a three-platform
   release: regenerate the checked-in clients with `raco rivet build`
   (`linux/GeneratedBackend.hpp`, `windows/GeneratedBackend.hpp`,
   `macos-host/Sources/RivetHost/GeneratedBackend.swift`) in the same
   commit — they are build inputs, never hand-edit them. User-visible
   strings live in `shared/i18n/{zh,en}.json`; the generated per-host
   tables must keep exact parity (`scripts/gen-win-strings.mjs --check`,
   `scripts/gen-linux-strings.mjs --check`,
   `scripts/gen-swift-strings.mjs --check`).
2. **The schema is checked, not vibes.** `raco rivet schema check
   rivet-schema.json --json` guards the app manifest contract; change
   `rivet-schema.json` and the consumers in the same commit.
3. **The storage contract is file-first.** `shared/spec/DATA-FORMAT.md`
   pins where every byte Booklight writes may live: documents open in
   place, annotations land in sidecars (or the central directory for
   existing profiles), the provider key stays in the OS credential store,
   and `settings.json` keeps `api_key` empty. New storage needs a spec
   row first; `scripts/check-file-first.mjs` guards the no-library
   regression.
4. **The update contract is fail-closed.** `shared/spec/UPDATE.md` pins
   the triggers (silent check throttled to once per 24 h, manual
   bypass), silent-mode behavior and the backend-verified Ed25519 feed
   (key id `booklight-2026-11`, keys: `scripts/update-keys.sh`). A
   failed verification aborts and the running version keeps working —
   never add an unsigned fallback path.
5. **Fail honestly.** Network failure / rate limit / throttle is silent
   in the auto path and reported in the manual path; a failed update
   verification aborts with the running version untouched. No silent
   fallbacks.
6. **Privacy is a feature.** The signed update feed and the
   user-configured LLM provider are the application's only network use.
   No telemetry, no accounts, document bytes are never copied or
   uploaded.
7. **Tests pin contracts.** If you change behavior a test covers, change
   the test in the same commit and say why (`raco test racket/`).

## Before you push

```bash
bash scripts/check-release-version.sh          # VERSION == rivet.rktd == updater
python3 -m unittest discover -s scripts/tests -p 'test_release_signing.py'
node scripts/gen-win-strings.mjs --check       # i18n tables == shared/i18n
node scripts/gen-linux-strings.mjs --check
node scripts/gen-swift-strings.mjs --check
node scripts/check-file-first.mjs              # no book-library regression
raco test racket/                              # core contract tests
raco rivet schema check rivet-schema.json --json
```

These mirror the CI `Contract preflight` and `racket-tests` jobs.
Platform hosts compile in their own CI jobs via `raco rivet build`; if
you touch `macos-host/`, `windows/` or `linux/`, say whether you were
able to build locally. The signing contract tests need OpenSSL 3 for
Ed25519 — macOS's system LibreSSL cannot generate the test keys, so put
a real OpenSSL 3 first on `PATH` (e.g. `/opt/homebrew/opt/openssl@3/bin`).

## Reporting issues

Include: OS + version, Booklight version (the About dialog),
installation shape (DMG / portable zip / MSI / AppImage / deb / rpm /
tar.gz), whether the document is PDF or EPUB, and steps to reproduce
with a minimal file when possible. For update failures, include the
error text from the dialog. Security issues go through
[SECURITY.md](SECURITY.md), not public issues.
