#!/usr/bin/env bash
# Generate the Ed25519 update-signing key pair for Booklight
# (contract: shared/spec/UPDATE.md).
#
#   scripts/update-keys.sh [private-key-path]
#
# Default private key location: ~/.booklight/update-signing-key.pem —
# outside the repo, never committed. The private half also lives in the
# keys vault (~/Sync/Keys booklight section); CI signs release manifests
# with the GitHub secret UPDATE_ED25519_PRIVATE_KEY_B64 (OneAsymmetricKey
# DER, base64 — the same forms scripts/make-update-manifest.sh consumes).
# The script prints every derived form:
#   - public SPKI DER, base64     -> app/version.rkt update-public-key-b64
#   - private OneAsymmetricKey DER, base64 -> GitHub secret + vault
# Rotation needs a release that trusts the NEXT key first (see the note
# above update-public-key-b64 in app/version.rkt; key id
# booklight-2026-10).
# Requires OpenSSL 3 (Ed25519; macOS ships LibreSSL, which cannot do it).

set -euo pipefail

KEY_PATH="${1:-$HOME/.booklight/update-signing-key.pem}"

# ---- locate an OpenSSL 3 -----------------------------------------------------
find_openssl() {
  local candidates=("${OPENSSL_BIN:-}" \
    /opt/homebrew/opt/openssl@3/bin/openssl \
    /usr/local/opt/openssl@3/bin/openssl \
    "$(command -v openssl || true)")
  for c in "${candidates[@]}"; do
    [ -n "$c" ] && [ -x "$c" ] || continue
    if "$c" version 2>/dev/null | grep -q "OpenSSL 3"; then
      echo "$c"; return 0
    fi
  done
  return 1
}

OPENSSL_BIN_VALUE="$(find_openssl)" || {
  echo "error: OpenSSL 3 not found; install with: brew install openssl@3" >&2
  exit 1
}

# ---- refuse to overwrite -----------------------------------------------------
if [ -e "$KEY_PATH" ]; then
  echo "error: $KEY_PATH already exists." >&2
  echo "Rotating keys needs a release that trusts the NEXT key first —" >&2
  echo "see the rotation note above update-public-key-b64 in
  app/version.rkt. Move the old file away if you really mean it." >&2
  exit 1
fi

mkdir -p "$(dirname "$KEY_PATH")"
umask 077

"$OPENSSL_BIN_VALUE" genpkey -algorithm ed25519 -out "$KEY_PATH"
PUB_SPKI_B64="$("$OPENSSL_BIN_VALUE" pkey -in "$KEY_PATH" -pubout -outform DER 2>/dev/null | base64)"
PRIV_DER_B64="$("$OPENSSL_BIN_VALUE" pkey -in "$KEY_PATH" -outform DER 2>/dev/null | base64)"

echo "private key: $KEY_PATH"
echo "public SPKI DER (base64, app/version.rkt update-public-key-b64): $PUB_SPKI_B64"
echo "private OneAsymmetricKey DER (base64, UPDATE_ED25519_PRIVATE_KEY_B64): $PRIV_DER_B64"
