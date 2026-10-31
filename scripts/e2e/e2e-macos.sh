#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo/.rivet/acceptance/macos"
fixture="$repo/racket/tests/fixtures/sample.epub"
app="$(find "$repo/dist" -type f -path '*/Contents/MacOS/*' -perm -u+x -print -quit)"
driver="$output/macos-ax"
home="$output/home-$$"
mkdir -p "$output" "$home"
[[ -n "$app" && -x "$app" && -f "$fixture" ]]
racket "$repo/scripts/acceptance/seed-annotation-recovery.rkt" "$home/Library/Application Support/site.jrtx.booklight" "$fixture"
racket "$repo/scripts/acceptance/seed-ai-preview.rkt" "$home/Library/Application Support/site.jrtx.booklight"
swiftc "$repo/scripts/e2e/macos-ax.swift" -o "$driver" -framework AppKit -framework ApplicationServices
HOME="$home" BOOKLIGHT_OPEN="$fixture" "$app" >"$output/application.log" 2>&1 &
app_pid=$!
stop_bounded() { local pid="$1"; kill -TERM "$pid" 2>/dev/null || true; for _ in $(seq 1 200); do kill -0 "$pid" 2>/dev/null || return 0; sleep 0.05; done; kill -KILL "$pid" 2>/dev/null || true; return 1; }
cleanup() { local status=$?; trap - EXIT; if kill -0 "$app_pid" 2>/dev/null; then stop_bounded "$app_pid" || true; wait "$app_pid" 2>/dev/null || true; fi; exit "$status"; }
trap cleanup EXIT
"$driver" --pid "$app_pid" --output "$output"
for _ in $(seq 1 200); do kill -0 "$app_pid" 2>/dev/null || break; sleep 0.05; done
if kill -0 "$app_pid" 2>/dev/null; then echo "Booklight did not exit after native termination" >&2; exit 1; fi
wait "$app_pid"
trap - EXIT
