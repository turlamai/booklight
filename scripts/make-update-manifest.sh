#!/usr/bin/env bash
# Build the Booklight update manifest (rivet format) over the final artifacts.
#
# Usage: scripts/make-update-manifest.sh <tag> <dist-dir> <key-der-path>
#   <tag>          release tag, e.g. v0.1.0 (must match the VERSION file)
#   <dist-dir>     directory containing the release artifacts, i.e. the names
#                  the release pipeline produces:
#                    booklight-<ver>-macos-arm64.zip   booklight-<ver>-macos-x64.zip
#                    booklight-<ver>-windows-x64.zip
#                    booklight-<ver>-linux-x64.tar.gz  booklight-<ver>-linux-arm64.tar.gz
#   <key-der-path> Ed25519 private key in DER (OneAsymmetricKey) form; the CI
#                  secret stores it base64-encoded.
#
# Overwrites <dist-dir>/update-manifest.json — the signed stable-channel
# manifest under the family-canonical feed name, which current installs pull.
# The same bytes are also published as <dist-dir>/update-stable.json, a
# byte-identical alias for pre-switch installs. One entry per macOS
# architecture, the Windows x64 portable zip, and the Linux x64/arm64
# tarballs. The signing, hashing, schema validation, and alias live in
# `raco rivet sign-manifest`; this wrapper owns only Booklight's artifact
# matrix and identity.
#
# Env overrides: RELEASE_ASSET_BASE_URL, RIVET_UPDATE_KEY_ID.
set -euo pipefail

TAG="${1:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
DIST="${2:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
KEY_PATH="${3:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
VERSION="${TAG#v}"
KEY_ID="${RIVET_UPDATE_KEY_ID:-booklight-2026-11}"
BASE_URL="${RELEASE_ASSET_BASE_URL:-https://github.com/turlamai/booklight/releases/download/$TAG}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$(cd "$DIST" && pwd)"
cd "$ROOT"

raco rivet check-release-version "$TAG"

BUILD="$(sed -nE 's/.*\(build \. ([0-9]+)\).*/\1/p' "$ROOT/rivet.rktd" | head -n1)"
[[ -n "$BUILD" ]] || {
  echo "error: could not read build from rivet.rktd" >&2
  exit 1
}
[[ -f "$KEY_PATH" ]] || {
  echo "error: missing signing key $KEY_PATH" >&2
  exit 1
}

raco rivet sign-manifest \
  --application-id site.jrtx.booklight \
  --version "$VERSION" \
  --build "$BUILD" \
  --channel stable \
  --base-url "$BASE_URL" \
  --key-id "$KEY_ID" \
  --private-key "$KEY_PATH" \
  --output "$DIST/update-manifest.json" \
  --alias "$DIST/update-stable.json" \
  --artifact "macos:arm64:zip:$DIST/booklight-$VERSION-macos-arm64.zip" \
  --artifact "macos:x64:zip:$DIST/booklight-$VERSION-macos-x64.zip" \
  --artifact "windows:x64:zip:$DIST/booklight-$VERSION-windows-x64.zip" \
  --artifact "linux:x64:targz:$DIST/booklight-$VERSION-linux-x64.tar.gz" \
  --artifact "linux:arm64:targz:$DIST/booklight-$VERSION-linux-arm64.tar.gz" \
  --artifact "linux:x64:appimage:$DIST/booklight-$VERSION-linux-x86_64.AppImage" \
  --artifact "linux:arm64:appimage:$DIST/booklight-$VERSION-linux-aarch64.AppImage" \
  --rollback-allowed

echo "manifest: $DIST/update-manifest.json + update-stable.json alias (macos arm64/x64 + windows x64 zips + linux x64/arm64 tar.gz + AppImages)"
