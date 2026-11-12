#!/bin/sh
# One demo run inside `make test` (QA TC-SRV-01): scripts/run_demo.sh with its logs and state in a temp
# directory, kept only on failure.
# usage: tests/m6_demo.sh BUILD_DIR   Env: ZTP_PORT (default 44555), SEED (default 42)
set -u
BIN=$1
T=${SUITE_DIR:-${RESULTS:+$RESULTS/m6_demo}}; T=${T:-${TMPDIR:-/tmp}/ztp-demo.$$}   # kept under RESULTS (make test)
mkdir -p "$T"
if DEMO_DIR="$T" PORT="${ZTP_PORT:-44555}" QUIET=1 BIN="$BIN" sh "$(dirname "$0")/../scripts/run_demo.sh"; then
    [ -n "${RESULTS:-}" ] || rm -rf "$T"
    exit 0
fi
echo "  (demo logs kept in $T/logs)"
exit 1
