#!/bin/sh
# raco distribute keeps its relative bin/ + lib/ layout; the glaze carrier
# assembles public/ at runtime from define-runtime-path inside lib/.
exec /app/booklight/bin/booklight "$@"