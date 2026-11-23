#!/usr/bin/env bash
# Release version preflight (#75 S6): VERSION == app/version.rkt == CHANGELOG.
# The rivet.rktd app manifest is retired with the native legs; the glaze
# carrier embeds its identity in app/version.rkt and the release notes gate
# lives in CHANGELOG.md (release.yml greps the tag's section).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
TAG="${1:-}"

fail() {
  echo "release preflight: $*" >&2
  exit 1
}

[[ -n "$VERSION" ]] || fail "VERSION is empty"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$ ]] || \
  fail "VERSION '$VERSION' is not a supported semantic version"

if [[ -n "$TAG" ]]; then
  TAG_VERSION="${TAG#v}"
  [[ "$TAG_VERSION" == "$VERSION" ]] || \
    fail "tag '$TAG' does not match VERSION '$VERSION'"
fi

# The backend updater embeds the release identity for the update feed.
grep -qF "(define app-version \"$VERSION\")" "$ROOT/app/version.rkt" || \
  fail "app/version.rkt app-version does not match VERSION '$VERSION'"

# The release notes must carry a section for this version (release.yml
# extracts it with grep '^## \[TAG\]').
grep -qF "## [$VERSION]" "$ROOT/CHANGELOG.md" || \
  fail "CHANGELOG.md has no '## [$VERSION]' section"

echo "release preflight: version $VERSION is aligned (VERSION == updater == CHANGELOG)"