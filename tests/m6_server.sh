#!/bin/sh
# M6 server cases outside the demo: TC-SRV-02 (per-device assignment, a serial override beating its model)
# and TC-SRV-10 (the server is SIGKILLed and restarted while a device restarts mid-upgrade).
# usage: tests/m6_server.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts
# Env: ZTP_PORT (default 43555; +1 = discovery).
set -u
SRV=$1
DEV=$2
MKA=$3
PORT=${ZTP_PORT:-43555}
DPORT=$((PORT + 1))
T=${RESULTS:+$RESULTS/m6_server}; T=${T:-${TMPDIR:-/tmp}/ztp-m6s.$$}   # kept under RESULTS (make test)
mkdir -p "$T/art"
pass=0
fail=0
SRV_PID=
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    if [ "$fail" -ne 0 ]; then echo "  (logs kept in $T)"; elif [ -z "${RESULTS:-}" ]; then rm -rf "$T"; fi
}
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }
row() { grep "^| $2 " "$1" 2>/dev/null | head -1 | awk -F'|' '{ for (i = 2; i <= 7; i++) { gsub(/^ +| +$/, "", $i) } print $3 "|" $4 "|" $5 "|" $7 }'; }

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
A="$T/art"
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": { "nat": { "grace_s": 180 } } },
  "models": {
    "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$A/fw_EDGE-GW-100_v1.1.bin", "config_version": 3,
                     "config_file": "$A/cfg_v3.json", "tenant": "cruise_ops" },
    "EDGE-GW-050": { "firmware": "v1.0", "firmware_file": "$A/fw_EDGE-GW-050_v1.0.bin", "config_version": 2,
                     "config_file": "$A/cfg_v2.json", "tenant": "cruise_ops", "notes": "Legacy device" } },
  "serials": {
    "ZTP-55AC90": { "config_version": 2, "config_file": "$A/cfg_v2.json", "nat": { "keepalive_interval_s": 10 } } } }
JSON

start_server() {   # start_server NAME DB [args...]
    name=$1; db=$2; shift 2
    "$SRV" --profile demo --set net.server_port=$PORT --set net.discovery_port=$DPORT --policy "$T/policy.json" \
        --set paths.log="$T/$name.log" --set paths.db="$T/$db.db" --set paths.dump="$T/$name.dump" "$@" \
        > "$T/$name.out" 2>&1 &
    SRV_PID=$!
    i=0
    while [ $i -lt 80 ] && ! has "$T/$name.log" "Server listening"; do sleep 0.1; i=$((i + 1)); done
    has "$T/$name.log" "Server listening" || bad "server $name did not start: $(cat "$T/$name.out")"
}
stop_server() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null; SRV_PID=; }
run_device() {     # run_device NAME SECS [args...]: DEV_RC
    name=$1; secs=$2; shift 2
    "$DEV" --profile demo --state-dir "$T/$name.state" --set paths.log="$T/$name.log" --set run.operational_hold_s=1 \
        --set discovery.discovery_port=$DPORT --discover static --server 127.0.0.1:$PORT "$@" >> "$T/$name.out" 2>&1 &
    DPID=$!
    ( sleep "$secs"; kill "$DPID" 2>/dev/null ) > /dev/null 2>&1 &
    wpid=$!
    wait "$DPID" 2>/dev/null
    DEV_RC=$?
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
}

echo "m6 server tests (ports $PORT/$DPORT)"

# TC-SRV-02: each device gets its model's assignment, or its serial override (serial > model > default)
start_server a a --dump-on-exit
run_device a1 40 --serial ZTP-93AF21 --model EDGE-GW-100 --fw v1.0
run_device a2 40 --serial ZTP-55AC90 --model EDGE-GW-100 --fw v1.1
run_device a3 40 --serial ZTP-19FE77 --model EDGE-GW-050 --fw v0.9
stop_server
[ "$(row "$T/a.dump" ZTP-93AF21)" = "Success|v1.1|v3|—" ] && ok || bad "TC-SRV-02: ZTP-93AF21 row '$(row "$T/a.dump" ZTP-93AF21)'"
[ "$(row "$T/a.dump" ZTP-55AC90)" = "Success|v1.1|v2|—" ] && ok || bad "TC-SRV-02: ZTP-55AC90 row '$(row "$T/a.dump" ZTP-55AC90)' (override: cfg v2)"
[ "$(row "$T/a.dump" ZTP-19FE77)" = "Success|v1.0|v2|Legacy device" ] && ok || bad "TC-SRV-02: ZTP-19FE77 row '$(row "$T/a.dump" ZTP-19FE77)'"
grep -q 'ZTP-55AC90\] Policy: EDGE-GW-100 -> fw v1.1, cfg 2, keepalive 10 s, grace 180 s (source: ' "$T/a.log" && ok \
    || bad "TC-SRV-02: the serial override was not applied to ZTP-55AC90"
grep -q 'ZTP-93AF21\] Policy: EDGE-GW-100 -> fw v1.1, cfg 3, keepalive 2 s, grace 180 s (source: ' "$T/a.log" && ok \
    || bad "TC-SRV-02: ZTP-93AF21 did not get its model's assignment and the tenant grace"
expect_line "$T/a2.log" "[VALIDATION] Configuration present (v2, sha ok)" "TC-SRV-02 (ZTP-55AC90 runs cfg v2)"

# TC-SRV-10: the server is SIGKILLed while ZTP-93AF21 waits to restart after the commit (AWAIT_RESUME), and
# restarted from its journal; the device's resume after its restart is accepted with no second REGISTER
start_server b1 b --test-hooks --first-session-id 7421
run_device b 40 --serial ZTP-93AF21 --model EDGE-GW-100 --fw v1.0 --test-hooks --restart-mode exit --restart-delay 5 &
BG=$!
i=0
while [ $i -lt 400 ] && ! has "$T/b1.log" "awaiting resume"; do sleep 0.05; i=$((i + 1)); done
has "$T/b1.log" "awaiting resume" && ok || bad "TC-SRV-10: the server never reached AWAIT_RESUME"
kill -9 "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=
start_server b2 b --dump-on-exit
wait "$BG"
has "$T/b.log" "Restart delayed by 5 s" && ok || bad "TC-SRV-10: the restart was not delayed"
run_device b 40 --serial ZTP-93AF21 --model EDGE-GW-100 --fw v1.0 --test-hooks --restart-mode exit --restart-delay 5
stop_server
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-SRV-10: device exit $DEV_RC"
expect_line "$T/b2.log" "Restored 1 journaled session(s) into IDLE_GRACE" "TC-SRV-10"
expect_line "$T/b2.log" "[sid=7421 ZTP-93AF21] Resume accepted" "TC-SRV-10"
expect_line "$T/b.log" "[UDP] Session resumed (session_id=7421) epoch=1" "TC-SRV-10"
expect_line "$T/b.log" "[STATE] Device is OPERATIONAL" "TC-SRV-10"
[ "$(grep -c '\[AUTH\] REGISTER:' "$T/b.log")" -eq 1 ] && ok || bad "TC-SRV-10: registered again"
[ "$(row "$T/b2.dump" ZTP-93AF21)" = "Success|v1.1|v3|—" ] && ok || bad "TC-SRV-10: row '$(row "$T/b2.dump" ZTP-93AF21)'"
[ "$(grep -c '^| ZTP-93AF21 ' "$T/b2.dump")" -eq 1 ] && ok || bad "TC-SRV-10: not exactly one row"

echo "m6 server tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
