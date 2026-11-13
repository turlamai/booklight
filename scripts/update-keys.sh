#!/usr/bin/env bash
# Generate the Ed25519 update-signing key pair for Booklight
# (contract: shared/spec/UPDATE.md).
#
#   scripts/update-keys.sh [private-key-path]
#
# Default private key location: ~/.booklight/update-signing-key.pem —
# outside the repo, never committed. `raco rivet update-keys` prints every
# derived form:
#   - public SPKI DER base64       -> app/version.rkt update-public-key-b64
#   - public SPKI hex and raw 32-byte base64 -> native verifiers
#   - private OneAsymmetricKey DER base64 -> GitHub secret + keys vault
# Rotation needs a release that trusts the NEXT key first (see the note
# above update-public-key-b64 in app/version.rkt).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_PATH="${1:-$HOME/.booklight/update-signing-key.pem}"
KEY_ID="$(sed -n 's/^(define update-key-id "\([^"]*\)".*/\1/p' "$ROOT/app/version.rkt")"
[[ "$KEY_ID" =~ ^[A-Za-z0-9._-]+$ ]] || {
  echo "error: could not read a valid update-key-id from app/version.rkt" >&2
  exit 1
}

exec raco rivet update-keys --key "$KEY_PATH" --key-id "$KEY_ID"
