#!/usr/bin/env bash
# Build update-manifest.json for a release and sign it (#75 S6).
#
#   scripts/make-update-manifest.sh <tag> <dist-dir> [key-path]
#
#   <tag>       release tag, e.g. v0.1.0 (must match the VERSION file)
#   <dist-dir>  directory containing the release artifacts, i.e. the names
#               the release pipeline produces:
#                 booklight-<ver>-macos-arm64.zip   booklight-<ver>-macos-x64.zip
#                 booklight-<ver>-windows-x64.zip
#                 booklight-<ver>-linux-x64.tar.gz
#                 booklight-<ver>-linux-arm64.tar.gz
#               (the Flatpak bundle is a human installer, not a feed entry;
#               Linux hosts update through the tar.gz)
#   <key-path>  Ed25519 private key PEM (default: $UPDATE_KEY_PATH, then
#               ~/.booklight/update-signing-key.pem)
#
# The payload is assembled here (five portable artifacts, Booklight identity);
# schema validation and the schema-1 Ed25519 wrapper live in
# `raco glaze manifest-sign`, which verifies with the same pure-Racket
# ed25519 code installed clients run. Signing itself shells out to OpenSSL 3
# (macOS ships LibreSSL, which cannot do Ed25519), so an OpenSSL 3 binary is
# located and put on PATH for the glaze signer.
set -euo pipefail

TAG="${1:?usage: make-update-manifest.sh <tag> <dist-dir> [key-path]}"
DIST="${2:?usage: make-update-manifest.sh <tag> <dist-dir> [key-path]}"
KEY="${3:-${UPDATE_KEY_PATH:-$HOME/.booklight/update-signing-key.pem}}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Git Bash hands out msys-style paths (/d/...), which Racket's path
# primitives misread as drive-relative; convert to Windows mixed form.
if command -v cygpath >/dev/null 2>&1; then
  ROOT="$(cygpath -m "$ROOT")"
  DIST="$(cygpath -m "$DIST")"
  KEY="$(cygpath -m "$KEY")"
fi

VERSION="${TAG#v}"
cd "$ROOT"
bash scripts/check-release-version.sh "$TAG"

APP_ID="$(sed -nE 's/^\(define app-identifier "([^"]+)"\).*/\1/p' "$ROOT/app/version.rkt" | head -n1)"
BUILD="$(sed -nE 's/^\(define app-build ([0-9]+)\).*/\1/p' "$ROOT/app/version.rkt" | head -n1)"
KEY_ID="$(sed -nE 's/^\(define update-key-id "([^"]+)"\).*/\1/p' "$ROOT/app/version.rkt" | head -n1)"
[[ -n "$APP_ID" && -n "$BUILD" && -n "$KEY_ID" ]] || {
  echo "error: could not read application id/build/key-id from app/version.rkt" >&2
  exit 1
}
[[ -f "$KEY" ]] || {
  echo "error: missing signing key $KEY (see scripts/update-keys.sh)" >&2
  exit 1
}

BASE_URL="${RELEASE_ASSET_BASE_URL:-https://github.com/turlamai/booklight/releases/download/$TAG}"

# Ed25519 needs OpenSSL 3+ (macOS ships LibreSSL, which cannot do it).
OPENSSL_BIN="${OPENSSL_BIN:-}"
if [[ -z "$OPENSSL_BIN" ]]; then
  for candidate in /opt/homebrew/opt/openssl@3/bin/openssl \
                   /usr/local/opt/openssl@3/bin/openssl openssl; do
    if command -v "$candidate" >/dev/null 2>&1 \
       && "$candidate" version 2>/dev/null | grep -q "OpenSSL 3"; then
      OPENSSL_BIN="$candidate"
      break
    fi
  done
fi
[[ -n "$OPENSSL_BIN" ]] || {
  echo "error: OpenSSL 3.x required for Ed25519 (brew install openssl@3," >&2
  echo "       or set OPENSSL_BIN=/path/to/openssl)" >&2
  exit 1
}
# glaze's signer invokes `openssl` from PATH.
export PATH="$(dirname "$OPENSSL_BIN"):$PATH"

PAYLOAD="$(mktemp "${TMPDIR:-/tmp}/booklight-payload-XXXXXX.json")"
command -v cygpath >/dev/null 2>&1 && PAYLOAD="$(cygpath -m "$PAYLOAD")"
trap 'rm -f "$PAYLOAD"' EXIT

PYTHON_BIN="${PYTHON_BIN:-}"
if [[ -z "$PYTHON_BIN" ]]; then
  for candidate in python3 python; do
    # Windows ships a python3 Store-alias stub that exits without running;
    # only accept an interpreter that actually executes.
    if command -v "$candidate" >/dev/null 2>&1 \
       && "$candidate" -c 'import sys' >/dev/null 2>&1; then
      PYTHON_BIN="$candidate"
      break
    fi
  done
fi
[[ -n "$PYTHON_BIN" ]] || { echo "error: python3 required to assemble the payload" >&2; exit 1; }

"$PYTHON_BIN" - "$DIST" "$VERSION" "$APP_ID" "$BUILD" "$BASE_URL" "$PAYLOAD" <<'PY'
import hashlib, json, os, sys, time

dist, version, app_id, build, base_url, out = sys.argv[1:7]
artifacts = []
for platform, arch, installer, suffix in [
    ("macos", "arm64", "zip", "macos-arm64.zip"),
    ("macos", "x64", "zip", "macos-x64.zip"),
    ("windows", "x64", "zip", "windows-x64.zip"),
    ("linux", "x64", "tar.gz", "linux-x64.tar.gz"),
    ("linux", "arm64", "tar.gz", "linux-arm64.tar.gz"),
]:
    name = "booklight-%s-%s" % (version, suffix)
    path = os.path.join(dist, name)
    with open(path, "rb") as source:
        data = source.read()
    artifacts.append({
        "platform": platform,
        "architecture": arch,
        "url": "%s/%s" % (base_url, name),
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
        "installer": installer,
        "arguments": [],
        "signature": None,
    })
payload = {
    "schema": 1,
    "application_id": app_id,
    "version": version,
    "build": int(build),
    "channel": "stable",
    "published_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "minimum_version": "0.0.0",
    "previous_version": None,
    "rollback_allowed": True,
    "rollout": 100,
    "artifacts": artifacts,
}
with open(out, "w", encoding="utf-8") as target:
    json.dump(payload, target, separators=(",", ":"))
print("payload: %d artifacts for %s %s" % (len(artifacts), app_id, version))
PY

raco glaze manifest-sign \
  --manifest "$PAYLOAD" \
  --key "$KEY" \
  --key-id "$KEY_ID" \
  --out "$DIST/update-manifest.json"

echo "manifest: $DIST/update-manifest.json (5 artifacts, key-id $KEY_ID)"