#!/bin/sh
# M3 integration tests: registration, the DB and its journal, idle/grace, startup checks.
# usage: tests/m3_register.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts path/to/peer_sim
# Env: ZTP_PORT (default 27555; +1 = discovery).
set -u
SRV=$1
DEV=$2
MKA=$3
SIM=$4
PORT=${ZTP_PORT:-27555}
DPORT=$((PORT + 1))
T=${TMPDIR:-/tmp}/ztp-m3.$$
mkdir -p "$T"
pass=0
fail=0
SRV_PID=

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    if [ "$fail" -eq 0 ]; then rm -rf "$T"; else echo "  (logs kept in $T)"; fi
}
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }
count() { grep -F -c -- "$2" "$1" 2>/dev/null || true; }
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }

# start_server NAME DBNAME [args...]: the DB (journal) is named separately so a restart can reuse it
start_server() {
    name=$1; dbn=$2; shift 2
    "$SRV" --profile demo --set net.server_port=$PORT --set net.discovery_port=$DPORT \
        --set paths.log="$T/$name.log" --set paths.db="$T/$dbn.jsonl" --set paths.dump="$T/$name.dump" "$@" \
        > "$T/$name.out" 2>&1 &
    SRV_PID=$!
    i=0
    while [ $i -lt 50 ] && ! has "$T/$name.log" "Server listening"; do sleep 0.1; i=$((i + 1)); done
    has "$T/$name.log" "Server listening" || bad "server $name did not start: $(cat "$T/$name.out")"
}
stop_server() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null; SRV_PID=; }
run_device() {
    name=$1; secs=$2; shift 2
    "$DEV" --profile demo --state-dir "$T/$name.state" --set paths.log="$T/$name.log" \
        --set discovery.discovery_port=$DPORT --discover static --server 127.0.0.1:$PORT "$@" > "$T/$name.out" 2>&1 &
    dpid=$!
    ( sleep "$secs"; kill "$dpid" 2>/dev/null ) &
    wpid=$!
    wait "$dpid"
    DEV_RC=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
}

echo "m3 registration tests (ports $PORT/$DPORT)"

# --- TC-AUTH-01/02, TC-SRV-03: a valid registration, the DB row, the dump and the journal ------------
start_server s1 db1
run_device d1 20 --serial ZTP-93AF21 -v
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-AUTH-02: device exit $DEV_RC"
expect_line "$T/d1.log" "[AUTH] REGISTER: session_id=" TC-AUTH-01
expect_line "$T/d1.log" "TLVs: SERIAL MODEL FW_VER HW_REV(A1) NONCE_REG NONCE_SRV" TC-AUTH-01
expect_line "$T/d1.log" "[AUTH] Registration successful" TC-AUTH-02
expect_line "$T/d1.log" "[AUTH] Assigned tenant: cruise_ops" TC-AUTH-02
grep -q '^stage=REGISTERED$' "$T/d1.state/device.state" && ok || bad "TC-AUTH-02: stage not persisted"
expect_line "$T/s1.log" "ZTP-93AF21] Registration successful" "TC-AUTH-02 (server)"
expect_line "$T/s1.log" "ZTP-93AF21] Assigned tenant: cruise_ops, fw v1.1, config 3" "TC-AUTH-02 (server)"
expect_line "$T/s1.log" "Policy: EDGE-GW-100 -> fw v1.1, cfg 3, keepalive 2 s, grace 30 s (source: profile:demo)" TC-AUTH-02
# TC-REL-11: an idle registered session: KEEPALIVE_ACKs and ACKs are never acknowledged (no ping-pong).
# The device ACKs only the server's reliable frames (HELLO_ACK, REGISTER_ACK: at most 2 standalone ACKs),
# however many idle keepalives follow.
n_ack=$(grep -c 'tx ACK seq=' "$T/d1.log" || true)
n_ka=$(grep -c 'tx KEEPALIVE seq=' "$T/d1.log" || true)
[ "$n_ack" -le 2 ] && [ "$n_ka" -ge 3 ] && ok || bad "TC-REL-11: device sent $n_ack ACKs for $n_ka keepalives"

# --- TC-AUTH-09: UNKNOWN_MODEL leaves a Failed row "Unknown model" -----------------------------------
run_device d2 20 --serial ZTP-999999 --model EDGE-GW-999
[ "$DEV_RC" -eq 1 ] && ok || bad "TC-AUTH-09: device exit $DEV_RC"

# --- TC-REL-04: the REGISTER_ACK is lost; the retransmitted REGISTER is a duplicate, processed once ---
stop_server
# A slower server RTO makes the device's REGISTER retransmission arrive before the server's own
# REGISTER_ACK retransmission, so the duplicate path is what recovers (otherwise it's a race).
start_server s2 db1 --test-hooks --drop-seq tx:2 --set rel.rto_min_ms=1500 --set rel.rto_initial_ms=1500
run_device d3 25 --serial ZTP-71DD08
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-REL-04: device exit $DEV_RC"
expect_line "$T/d3.log" "[UDP] Retransmitting seq=3 (timeout)" TC-REL-04
expect_line "$T/s2.log" "ZTP-71DD08] Duplicate packet seq=3 ignored" TC-REL-04
n=$(count "$T/s2.log" "ZTP-71DD08] Registration successful")
[ "$n" -eq 1 ] && ok || bad "TC-REL-04: $n registrations processed, want 1"
stop_server   # SIGTERM: dump + summary + compaction

# --- TC-SRV-03: dump fields; the journal survives the restart (one row per serial) ----------------
D="$T/s2.dump"
expect_line "$D" "| Device ID  | Registration Status | Firmware Assigned | Config Version | Registration Time   | Notes            |" TC-SRV-03
grep -q '^| ZTP-93AF21 | Success  *| v1.1  *| v3  *| 20[0-9-]* [0-9:]* | —  *|$' "$D" && ok || bad "TC-SRV-03: ZTP-93AF21 row"
grep -q '^| ZTP-999999 | Failed  *| —  *| —  *| 20[0-9-]* [0-9:]* | Unknown model  *|$' "$D" && ok || bad "TC-AUTH-09: Unknown model row"
grep -q '^| ZTP-71DD08 | Success ' "$D" && ok || bad "TC-SRV-03: ZTP-71DD08 row (from before the restart)"
expect_line "$D" "Registered successfully : 2" TC-SRV-03
expect_line "$D" "Failed registration     : 1" TC-SRV-03
grep -q '"ev":"RECORD","serial":"ZTP-71DD08".*"attempts":1,' "$T/db1.jsonl" && ok || bad "TC-REL-04: attempts != 1 in the journal"

# --- restart: journaled sessions come back in IDLE_GRACE, then expire; a resume gets SESSION_EXPIRED ---
start_server s3 db1 --set nat.grace_s=12
expect_line "$T/s3.log" "Restored 2 journaled session(s) into IDLE_GRACE" "session restore"
i=0
while [ $i -lt 150 ] && ! has "$T/s3.log" "Grace period over"; do sleep 0.1; i=$((i + 1)); done
expect_line "$T/s3.log" "Grace period over (12 s): session deleted, DB record kept" "grace expiry"
sid=$(sed -n 's/^session_id=//p' "$T/d1.state/device.state")
"$SIM" --server 127.0.0.1:$PORT resume-expired "$sid" ZTP-93AF21 > "$T/exp.sim" 2>&1 && ok || bad "SESSION_EXPIRED: $(cat "$T/exp.sim")"
stop_server
expect_line "$T/s3.dump" "Registered successfully : 2" "DB kept after session deletion"

# --- idle detection on a live registered session ---------------------------------------------------
start_server s4 db4 --set nat.grace_s=12
run_device d4 20 --serial ZTP-1D1E00 --set run.operational_hold_s=1
i=0
while [ $i -lt 100 ] && ! has "$T/s4.log" "ZTP-1D1E00] Session idle, entering grace"; do sleep 0.1; i=$((i + 1)); done
expect_line "$T/s4.log" "[NAT] [sid=" "idle detection"
expect_line "$T/s4.log" "ZTP-1D1E00] Session idle, entering grace" "idle detection"
stop_server

# --- TC-AUTH-12: known model, no policy -> NO_POLICY at once, Failed row "No policy" ----------------
"$MKA" --keys "$T/keys200.json" EDGE-GW-100 EDGE-GW-050 EDGE-GW-200 > /dev/null
start_server s5 db5 --keys "$T/keys200.json"
run_device d5 20 --serial ZTP-200200 --model EDGE-GW-200
[ "$DEV_RC" -eq 1 ] && ok || bad "TC-AUTH-12: device exit $DEV_RC"
expect_line "$T/d5.log" "[AUTH] Registration failed: NO_POLICY" TC-AUTH-12
expect_line "$T/d5.log" "[STATE] Device is FAILED (reason: NO_POLICY)" TC-AUTH-12
n=$(count "$T/d5.log" "[AUTH] REGISTER:")
[ "$n" -eq 1 ] && ok || bad "TC-AUTH-12: $n REGISTERs, want 1 (no retries)"
stop_server
grep -q '^| ZTP-200200 | Failed .*| No policy  *|$' "$T/s5.dump" && ok || bad "TC-AUTH-12: DB row"

# --- TC-CFG-09: startup checks ------------------------------------------------------------------------
sed 's|artifacts/fw_EDGE-GW-100_v1.1.bin|artifacts/missing.bin|' conf/policy.json > "$T/policy_missing.json"
"$SRV" --set paths.log="$T/c1.log" --set paths.db="$T/c.jsonl" --policy "$T/policy_missing.json" --run-for 1 > "$T/c1.out" 2>&1
[ $? -eq 78 ] && ok || bad "TC-CFG-09: missing firmware_file did not exit 78"
expect_line "$T/c1.out" "firmware_file" "TC-CFG-09 message"
"$MKA" --keys "$T/keys_nomodel.json" EDGE-GW-100 > /dev/null
"$SRV" --set paths.log="$T/c2.log" --set paths.db="$T/c.jsonl" --keys "$T/keys_nomodel.json" --run-for 1 > "$T/c2.out" 2>&1
[ $? -eq 78 ] && ok || bad "TC-CFG-09: a policy model without a key did not exit 78"
expect_line "$T/c2.out" "model EDGE-GW-050 has no key" "TC-CFG-09 message"

echo "m3 registration tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
