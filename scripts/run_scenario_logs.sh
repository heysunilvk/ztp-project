#!/bin/sh
# Committed log evidence for two behaviours of the PDF that the demo and Appendix A runs don't show
# (README §14.1, logs/README.md). `make scenario-logs`; about 1 minute; writes logs/scenarios/.
#  1. generated_serial/  PDF §1 "Generate or load a soft serial": a device with no configured serial generates
#     one on first boot ("[BOOT] Soft serial generated", the PDF's example line), persists it, and loads the
#     same serial on the next boot ("Soft serial loaded"), which resumes the session instead of registering
#     again (TC-DISC-01/02).
#  2. session_timeout/   PDF §4 "Session timeout detection": the server stops responding (SIGSTOP) while the
#     device is OPERATIONAL; the device detects the timeout, tries to resume with backoff, and resumes the same
#     session once the server answers again (SIGCONT), without registering again (TC-NAT-03).
# usage: scripts/run_scenario_logs.sh   (from the repository root, after `make`)
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-build}
OUT=${SCENARIO_LOGS:-logs/scenarios}
PORT=${PORT:-38555}
DPORT=$((PORT + 1))
W=${TMPDIR:-/tmp}/ztp-scenarios.$$
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }
wait_line() { i=0; while [ $i -lt $(($3 * 10)) ]; do has "$1" "$2" && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }
mkdir -p "$W" "$OUT/generated_serial" "$OUT/session_timeout"
"$BIN/mkartifacts" --out artifacts > /dev/null || { echo "mkartifacts failed"; exit 1; }
SRV_PID=
start_server() {   # start_server LOGFILE
    rm -f "$W/srv.db"
    "$BIN/ztp-server" --profile demo --set net.server_port=$PORT --set net.discovery_port=$DPORT --set paths.log="$1" \
        --set paths.db="$W/srv.db" --set paths.dump="$W/srv.dump" > "$W/srv.out" 2>&1 &
    SRV_PID=$!
    wait_line "$1" "Server listening" 10 || { bad "server did not start"; cat "$W/srv.out"; }
}
stop_server() { [ -n "$SRV_PID" ] && kill -INT "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null; SRV_PID=; }
device() {         # device STATE_DIR LOGFILE [args...]: no --serial, so the serial is generated or loaded
    st=$1; lg=$2; shift 2
    "$BIN/ztp-device" --profile demo --state-dir "$st" --set paths.log="$lg" --set discovery.discovery_port=$DPORT \
        --discover static --server 127.0.0.1:$PORT --model EDGE-GW-100 --fw v1.1 "$@" > "$W/dev.out" 2>&1
}

# 1. generated serial: first boot, then a restart that loads it
G=$OUT/generated_serial
rm -f "$G"/*.log
start_server "$G/server.log"
device "$W/g.state" "$G/device_first_boot.log" --set run.operational_hold_s=1
device "$W/g.state" "$G/device_second_boot.log" --set run.operational_hold_s=1
stop_server
has "$G/device_first_boot.log" "[BOOT] Soft serial generated" && ok || bad "no 'Soft serial generated' on the first boot"
has "$G/device_second_boot.log" "[BOOT] Soft serial loaded" && ok || bad "no 'Soft serial loaded' on the second boot"
s1=$(sed -n 's/.*\[BOOT\] Soft serial: \(ZTP-[0-9A-F]*\).*/\1/p' "$G/device_first_boot.log" | head -1)
s2=$(sed -n 's/.*\[BOOT\] Soft serial: \(ZTP-[0-9A-F]*\).*/\1/p' "$G/device_second_boot.log" | head -1)
[ -n "$s1" ] && [ "$s1" = "$s2" ] && ok || bad "serial changed across the restart: '$s1' then '$s2'"
has "$G/device_first_boot.log" "[STATE] Device is OPERATIONAL" && ok || bad "first boot: not OPERATIONAL"
# the second boot finds stage OPERATIONAL saved: it resumes the same session (no REGISTER) and stays operational
has "$G/device_second_boot.log" "[UDP] Session resumed (session_id=" && ok || bad "second boot: no resume"
grep -q '\[AUTH\] REGISTER:' "$G/device_second_boot.log" && bad "second boot: registered again" || ok

# 2. session timeout detection and recovery
S=$OUT/session_timeout
rm -f "$S"/*.log
start_server "$S/server.log"
"$BIN/ztp-device" --profile demo --state-dir "$W/t.state" --set paths.log="$S/device.log" --set discovery.discovery_port=$DPORT \
    --discover static --server 127.0.0.1:$PORT --model EDGE-GW-100 --fw v1.1 --set run.operational_hold_s=40 \
    > "$W/devt.out" 2>&1 &
DPID=$!
wait_line "$S/device.log" "[STATE] Device is OPERATIONAL" 30 || bad "session_timeout: never OPERATIONAL"
kill -STOP "$SRV_PID"
wait_line "$S/device.log" "[NAT] Session timeout detected" 15 && ok || bad "session_timeout: no timeout detected"
wait_line "$S/device.log" "Resuming session" 15 && ok || bad "session_timeout: no resume attempt"
sleep 2
kill -CONT "$SRV_PID"
wait_line "$S/device.log" "[UDP] Session resumed (session_id=" 30 && ok || bad "session_timeout: not resumed"
sleep 3
kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null
stop_server
[ "$(grep -c '\[AUTH\] REGISTER:' "$S/device.log")" -eq 1 ] && ok || bad "session_timeout: registered again"

rm -rf "$W"
echo "scenario logs: $pass passed, $fail failed (written to $OUT)"
[ "$fail" -eq 0 ]
