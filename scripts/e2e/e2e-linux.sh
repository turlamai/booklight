#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo/.rivet/acceptance/linux"
fixture="$repo/racket/tests/fixtures/sample.epub"
executable="$(find "$repo/dist" -type f -name RivetHost -perm -u+x -print -quit)"
mkdir -p "$output"
[[ -n "$executable" && -x "$executable" && -f "$fixture" ]]
fixture_hash_before="$(sha256sum "$fixture" | awk '{print $1}')"
export HOME="$output/home-$$"
export XDG_DATA_HOME="$output/data-$$"
export XDG_CONFIG_HOME="$output/config-$$"
export XDG_CACHE_HOME="$output/cache-$$"
export XDG_RUNTIME_DIR="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/booklight-acceptance-runtime-$$"
export DISPLAY=:99
export GDK_BACKEND=x11 GTK_A11Y=atspi NO_AT_BRIDGE=0 LANG=C.UTF-8
# GitHub-hosted runners deny the user namespace that WebKitGTK's bubblewrap
# sandbox requires. Limit the escape hatch to this packaged acceptance process;
# production launches keep WebKitGTK's sandbox enabled.
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
unset WAYLAND_DISPLAY
mkdir -p "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
racket "$repo/scripts/acceptance/seed-annotation-recovery.rkt" "$XDG_CONFIG_HOME/site.jrtx.booklight" "$fixture"
racket "$repo/scripts/acceptance/seed-ai-preview.rkt" "$XDG_CONFIG_HOME/site.jrtx.booklight"
# The journey drives real input synthesis (combo popups, keyboard selection),
# which AT-SPI only delivers through XTest under X11 — headless Wayland has
# no input injection and swallows AT-SPI press actions on popovers. Xvfb
# presents a desktop wide enough for the real 1280x860 window: a clipped
# output gives the inner paned a zero-width allocation, GTK never realizes
# the AI sidebar, and its whole AT-SPI subtree disappears.
Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp >"$output/xvfb.log" 2>&1 &
xvfb_pid=$!; app_pid=""; atspi_pid=""
stop_bounded() { local pid="$1"; kill -TERM "$pid" 2>/dev/null || true; for _ in $(seq 1 200); do kill -0 "$pid" 2>/dev/null || return 0; sleep 0.05; done; kill -KILL "$pid" 2>/dev/null || true; return 1; }
cleanup() { local status=$?; trap - EXIT; [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null && stop_bounded "$app_pid" || true; [[ -n "$atspi_pid" ]] && kill -0 "$atspi_pid" 2>/dev/null && kill -TERM "$atspi_pid" || true; kill -TERM "$xvfb_pid" 2>/dev/null || true; wait "$app_pid" "$atspi_pid" "$xvfb_pid" 2>/dev/null || true; exit "$status"; }
trap cleanup EXIT
for _ in $(seq 1 100); do [[ -S /tmp/.X11-unix/X99 ]] && break; kill -0 "$xvfb_pid" 2>/dev/null; sleep 0.05; done
[[ -S /tmp/.X11-unix/X99 ]]
launcher="$(command -v at-spi-bus-launcher || true)"; [[ -n "$launcher" ]] || launcher=/usr/libexec/at-spi-bus-launcher
"$launcher" --launch-immediately >"$output/at-spi-bus.log" 2>&1 & atspi_pid=$!
for _ in $(seq 1 100); do gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress >/dev/null 2>&1 && break; kill -0 "$atspi_pid" 2>/dev/null; sleep 0.05; done
export AT_SPI_BUS_ADDRESS="$(gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress | sed -E "s/^\('([^']+)',\)$/\1/")"
cd "$repo"; "$executable" "$fixture" >"$output/application.log" 2>&1 & app_pid=$!
/usr/bin/python3 "$repo/scripts/e2e/linux-atspi.py" --output "$output"
stop_bounded "$app_pid" || true; wait "$app_pid" 2>/dev/null || true; app_pid=""

# ---- close-guard journey: unsaved PDF page edits must survive close ----
guard_fixture="$output/sample-guard-$$.pdf"
racket "$repo/scripts/acceptance/seed-sample-pdf.rkt" "$guard_fixture"
guard_hash_before="$(sha256sum "$guard_fixture" | awk '{print $1}')"
cd "$repo"; "$executable" "$guard_fixture" >"$output/application-guard.log" 2>&1 & app_pid=$!
/usr/bin/python3 "$repo/scripts/e2e/linux-atspi-close-guard.py" --output "$output"
stop_bounded "$app_pid" || true; wait "$app_pid" 2>/dev/null || true; app_pid=""
[[ "$guard_hash_before" == "$(sha256sum "$guard_fixture" | awk '{print $1}')" ]]
trap - EXIT

# Opening a file must not create a managed catalog, cover cache or source edits.
[[ ! -e "$XDG_CONFIG_HOME/site.jrtx.booklight/library.json" ]]
[[ ! -d "$XDG_CONFIG_HOME/site.jrtx.booklight/covers" ]]
[[ "$fixture_hash_before" == "$(sha256sum "$fixture" | awk '{print $1}')" ]]
kill -TERM "$atspi_pid" "$xvfb_pid" 2>/dev/null || true; wait "$atspi_pid" "$xvfb_pid" 2>/dev/null || true
