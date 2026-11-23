#!/usr/bin/env bash
# Real-window carrier verify on a headless Linux runner. Session bus on a
# /run/user socket: WebKitGTK's sandboxed web process reaches the bus via
# xdg-dbus-proxy, which only exposes /run, not /tmp (WebKit bug 246159).
set -euo pipefail
export G_MESSAGES_DEBUG="${G_MESSAGES_DEBUG:-all}"
export WEBKIT_DISABLE_DMABUF_RENDERER=1
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
mkdir -p "$XDG_RUNTIME_DIR"
BUS_INFO="$(mktemp)"
dbus-daemon --session --fork \
  --address="unix:path=$XDG_RUNTIME_DIR/booklight-host-bus" \
  --print-address=1 --print-pid=1 > "$BUS_INFO"
DBUS_SESSION_BUS_ADDRESS="$(sed -n 1p "$BUS_INFO")"
export DBUS_SESSION_BUS_ADDRESS
BUS_PID="$(sed -n 2p "$BUS_INFO")"
status=0
xvfb-run -a racket host/verify.rkt || status=$?
kill "$BUS_PID" 2>/dev/null || true
rm -f "$BUS_INFO" "$XDG_RUNTIME_DIR/booklight-host-bus"
exit "$status"