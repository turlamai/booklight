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
#   <key-der-path> Ed25519 private key in DER (SubjectPublicKeyInfo/OneAsymmetricKey)
#                  form; the CI secret stores it base64-encoded.
#
# Overwrites <dist-dir>/update-stable.json — the signed stable-channel
# manifest (family format: schema + base64 payload + signature block, as
# written by rivet's own signer). The same bytes are also published as
# <dist-dir>/update-manifest.json, the family-standard feed name: existing
# installs keep pulling update-stable.json, while anything probing the
# standard name gets the identical manifest instead of a 404. Host code
# switching to the standard name is a later, separate change. One artifact
# entry per macOS architecture, the Windows x64 portable zip and the Linux
# x64/arm64 tarballs; clients pick theirs via rivet/distribution's platform
# + architecture filter, so every entry must be present and correctly
# named. The feed serves the portable zips (the DMGs and the MSI stay on
# the release for human download).
#
# Env overrides: RELEASE_ASSET_BASE_URL, RIVET_UPDATE_KEY_ID.

set -euo pipefail

TAG="${1:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
DIST="${2:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
KEY_PATH="${3:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
VERSION="${TAG#v}"
KEY_ID="${RIVET_UPDATE_KEY_ID:-booklight-2026-10}"
BASE_URL="${RELEASE_ASSET_BASE_URL:-https://github.com/turlamai/booklight/releases/download/$TAG}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- verify tag/version alignment -------------------------------------------
[[ "$VERSION" == "$(tr -d '[:space:]' < "$ROOT/VERSION")" ]] || {
  echo "error: tag $TAG does not match VERSION '$(cat "$ROOT/VERSION")'" >&2; exit 1; }
RKTD_VERSION="$(grep -o '"[0-9]*\.[0-9]*\.[0-9]*"' "$ROOT/rivet.rktd" | head -n1 | tr -d '"')"
if [ "$VERSION" != "$RKTD_VERSION" ]; then
  echo "error: tag $VERSION != rivet.rktd version $RKTD_VERSION" >&2
  exit 1
fi

for artifact in "$DIST/booklight-$VERSION-macos-arm64.zip" \
                "$DIST/booklight-$VERSION-macos-x64.zip" \
                "$DIST/booklight-$VERSION-windows-x64.zip" \
                "$DIST/booklight-$VERSION-linux-x64.tar.gz" \
                "$DIST/booklight-$VERSION-linux-arm64.tar.gz"; do
  if [ ! -f "$artifact" ]; then
    echo "error: missing installer: $artifact" >&2
    exit 1
  fi
done

# ---- build + sign the manifest with rivet's own signer -----------------------
MANIFEST="$DIST/update-stable.json"

SCRIPT="$(mktemp /tmp/booklight-manifest-XXXXXX.rkt)"
trap 'rm -f "$SCRIPT"' EXIT

cat > "$SCRIPT" <<RKT
#lang racket/base
(require rivet/distribution
         racket/date
         racket/file
         racket/format)
(define version "$VERSION")
(define base-url "$BASE_URL")
(define key-id "$KEY_ID")
(define dist (path->complete-path "$DIST"))
(define key-path (path->complete-path "$KEY_PATH"))
(define build (hash-ref (file->value (build-path (path->complete-path "$ROOT") "rivet.rktd")) 'build))

(define (artifact platform architecture file installer)
  (define path (build-path dist file))
  (unless (file-exists? path)
    (error 'make-update-manifest "missing installer: ~a" path))
  (update-artifact platform architecture
                   (string-append base-url "/" file)
                   (sha256-file/hex path)
                   (file-size path)
                   installer
                   '()
                   #f))

(define manifest
  (update-manifest "site.jrtx.booklight"
                   version
                   build
                   'stable
                   ;; published-at: RFC 3339, second precision, UTC
                   (let ([d (seconds->date (current-seconds) #f)])
                     (format "~a-~a-~aT~a:~a:~aZ"
                             (date-year d)
                             (~r (date-month d) #:min-width 2 #:pad-string "0")
                             (~r (date-day d) #:min-width 2 #:pad-string "0")
                             (~r (date-hour d) #:min-width 2 #:pad-string "0")
                             (~r (date-minute d) #:min-width 2 #:pad-string "0")
                             (~r (date-second d) #:min-width 2 #:pad-string "0")))
                   "0.0.0"
                   #f
                   #t
                   100
                   (list (artifact 'macos 'arm64
                                   (format "booklight-~a-macos-arm64.zip" version) 'zip)
                         (artifact 'macos 'x64
                                   (format "booklight-~a-macos-x64.zip" version) 'zip)
                         (artifact 'windows 'x64
                                   (format "booklight-~a-windows-x64.zip" version) 'zip)
                         ;; Linux updates through the tar.gz; the host's
                         ;; install-shape dispatch layers the deb/rpm hint
                         ;; and the AppImage sibling swap on top.
                         (artifact 'linux 'x64
                                   (format "booklight-~a-linux-x64.tar.gz" version) 'targz)
                         (artifact 'linux 'arm64
                                   (format "booklight-~a-linux-arm64.tar.gz" version) 'targz))))

;; write-signed-manifest validates the struct against the manifest schema
;; before signing, so a malformed manifest fails the release instead of
;; shipping something every client would reject.
(call-with-output-file (build-path dist "update-stable.json")
  #:exists 'truncate/replace
  (lambda (out)
    (write-signed-manifest manifest
                           (read-ed25519-private-key key-path)
                           key-id
                           out)
    (newline out)))
(printf "manifest: ~a (~a artifacts, key-id ~a)\\n"
        (build-path dist "update-stable.json")
        (length (update-manifest-artifacts manifest))
        key-id)
RKT

# rivet must be installed for the signer; the publish job links a checkout
racket "$SCRIPT"

# Family-standard alias: identical bytes under the name the rest of the
# family probes (update-manifest.json), so the family feed check stops 404
# while existing stable-name installs keep working untouched.
cp "$DIST/update-stable.json" "$DIST/update-manifest.json"

echo "manifest: $MANIFEST + update-manifest.json alias (macos arm64/x64 + windows x64 zips + linux x64/arm64 tar.gz)"
