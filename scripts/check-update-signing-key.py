#!/usr/bin/env python3
"""Fail closed unless the Ed25519 release private key matches the app trust root."""
import argparse
import base64
import hmac
import pathlib
import re
import subprocess
import sys


def public_key_from_app(source: str) -> bytes:
    match = re.search(r'\(define\s+update-public-key-b64\s+"([A-Za-z0-9+/=]+)"\s*\)', source)
    if not match:
        raise ValueError("app/version.rkt has no embedded update public key")
    try:
        return base64.b64decode(match.group(1), validate=True)
    except ValueError as exc:
        raise ValueError("app/version.rkt contains invalid public-key base64") from exc


def verify(private_der: bytes, expected_public_der: bytes) -> None:
    if not private_der:
        raise ValueError("release Ed25519 private key is empty")
    result = subprocess.run(
        ["openssl", "pkey", "-inform", "DER", "-pubout", "-outform", "DER"],
        input=private_der, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode != 0 or not result.stdout:
        raise ValueError("release Ed25519 private key is invalid or unreadable")
    if not hmac.compare_digest(result.stdout, expected_public_der):
        raise ValueError("release signing key does not match app/version.rkt trust root")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("private_key_der", type=pathlib.Path)
    parser.add_argument("--app-version", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1] / "app/version.rkt")
    args = parser.parse_args()
    try:
        verify(args.private_key_der.read_bytes(),
               public_key_from_app(args.app_version.read_text(encoding="utf-8")))
    except (OSError, ValueError) as exc:
        print(f"release signing preflight failed: {exc}", file=sys.stderr)
        return 1
    print("release signing key matches the bundled update trust root")
    return 0


if __name__ == "__main__":
    sys.exit(main())
