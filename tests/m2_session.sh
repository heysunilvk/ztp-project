#!/bin/sh
# M2 integration tests: real ztp-server and ztp-device processes on loopback (QA §5.2, §5.3, §5.5).
# usage: tests/m2_session.sh path/to/ztp-server path/to/ztp-device
# Env: ZTP_PORT (default 25555; discovery uses +1), BCAST=<directed broadcast address> to run TC-DISC-05.
set -u
SRV=$1
DEV=$2
PORT=${ZTP_PORT:-25555}
DPORT=$((PORT + 1))
T=${TMPDIR:-/tmp}/ztp-m2.$$
mkdir -p "$T"
pass=0
fail=0
SRV_PID=

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    if [ "$fail" -eq 0 ]; then rm -rf "$T"; else echo "  (logs kept in $T)"; fi
}
trap cleanup EXIT

ok()   { pass=$((pass + 1)); }
bad()  { echo "  FAIL: $1"; fail=$((fail + 1)); }
has()  { grep -F -q -- "$2" "$1" 2>/dev/null; }                 # has FILE TEXT
count() { grep -F -c -- "$2" "$1" 2>/dev/null || true; }         # count FILE TEXT
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }

COMMON="--set net.server_port=$PORT --set net.discovery_port=$DPORT"
DCOMMON="--set discovery.discovery_port=$DPORT --set discovery.server=127.0.0.1:$PORT"

# start_server NAME [args...]: server in the background, logging to $T/NAME.log
start_server() {
    name=$1; shift
    "$SRV" --profile demo $COMMON --set net.mcast_iface_addr=127.0.0.1 --set paths.log="$T/$name.log" "$@" \
        > "$T/$name.out" 2>&1 &
    SRV_PID=$!
    i=0
    while [ $i -lt 50 ] && ! has "$T/$name.log" "Server listening"; do sleep 0.1; i=$((i + 1)); done
    has "$T/$name.log" "Server listening" || bad "server $name did not start: $(cat "$T/$name.out")"
}
stop_server() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null
    SRV_PID=
}

# run_device NAME SECS [args...]: foreground device with a watchdog; sets DEV_RC; log is $T/NAME.log
run_device() {
    name=$1; secs=$2; shift 2
    "$DEV" --profile demo $DCOMMON --state-dir "$T/$name.state" --set paths.log="$T/$name.log" "$@" \
        > "$T/$name.out" 2>&1 &
    dpid=$!
    ( sleep "$secs"; kill "$dpid" 2>/dev/null ) &
    wpid=$!
    wait "$dpid"
    DEV_RC=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
}

echo "m2 session tests (ports $PORT/$DPORT)"

# --- TC-SESS-01, TC-DISC-03: static handshake, path probe -------------------------------------
start_server s1
run_device d1 20 --serial ZTP-93AF21 --discover static
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-SESS-01: device exit $DEV_RC"
expect_line "$T/d1.log" "[DISCOVERY] Using static server IP 127.0.0.1:$PORT" TC-DISC-03
expect_line "$T/d1.log" "[DISCOVERY] Provisioning server found 127.0.0.1:$PORT (static)" TC-DISC-03
expect_line "$T/d1.log" "[UDP] HELLO sent (seq=1)" TC-SESS-01
expect_line "$T/d1.log" "[NAT] Keepalive sent" TC-SESS-01
expect_line "$T/d1.log" "[NAT] Keepalive acknowledged" TC-SESS-01
sid=$(sed -n 's/.*\[UDP\] Session established (session_id=\([0-9]*\)).*/\1/p' "$T/d1.log" | head -1)
if [ -n "$sid" ] && [ "$sid" != 0 ]; then ok; else bad "TC-SESS-01: no session id in device log"; fi
expect_line "$T/s1.log" "[UDP] [sid=$sid ZTP-93AF21] Session established" "TC-SESS-01 (server, same sid)"
expect_line "$T/s1.log" "[NAT] [sid=$sid ZTP-93AF21] Keepalive (path probe) acknowledged" TC-SESS-01
expect_line "$T/s1.log" "HELLO seq=1 received (serial ZTP-93AF21, EDGE-GW-100, v1.0), HELLO_ACK seq=1 sent" TC-SESS-01

# --- TC-DISC-01/02: serial origin and identity ------------------------------------------------
run_device d2 20
expect_line "$T/d2.log" "[BOOT] Soft serial generated" TC-DISC-01
serial=$(sed -n 's/.*\[BOOT\] Soft serial: \(ZTP-[0-9A-F]\{6\}\)$/\1/p' "$T/d2.log" | head -1)
[ -n "$serial" ] && ok || bad "TC-DISC-01: serial line does not match ^ZTP-[0-9A-F]{6}$"
grep -q "^serial=$serial\$" "$T/d2.state/device.state" && ok || bad "TC-DISC-01: serial not persisted"
cp "$T/d2.state/device.state" "$T/state.before"
run_device d2 20
expect_line "$T/d2.log" "[BOOT] Soft serial loaded" "TC-DISC-02 (a)"
[ "$(grep -c "Soft serial: $serial" "$T/d2.log")" -eq 2 ] && ok || bad "TC-DISC-02 (a): serial changed"
expect_line "$T/d1.log" "[BOOT] Soft serial assigned (configuration)" "TC-DISC-02 (b)"
run_device d2 20 --serial ZTP-000001
[ "$DEV_RC" -eq 78 ] && ok || bad "TC-DISC-02 (c): identity change exit $DEV_RC, want 78"

# --- TC-SESS-02: first HELLO lost; one half-open session ----------------------------------------
run_device d3 20 --serial ZTP-71DD08 --discover static --test-hooks --drop-seq tx:1
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-SESS-02: device exit $DEV_RC"
expect_line "$T/d3.log" "[UDP] Retransmitting seq=1 (timeout)" TC-SESS-02
expect_line "$T/d3.log" "[ERROR] TEST HOOK ACTIVE: --drop-seq" TC-SESS-02
n=$(count "$T/s1.log" "HELLO seq=1 received (serial ZTP-71DD08")
[ "$n" -eq 1 ] && ok || bad "TC-SESS-02: $n sessions for ZTP-71DD08, want 1"
stop_server

# --- TC-SESS-03: first HELLO_ACK lost; served from the cache ------------------------------------
# The server's slower initial RTO makes the device's HELLO retransmission arrive first, so the
# cached-HELLO_ACK path is what recovers (otherwise the server's own retransmission may win the race).
start_server s3 --test-hooks --drop-seq tx:1 --set rel.rto_initial_ms=2000
run_device d4 20 --serial ZTP-55AC90 --discover static
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-SESS-03: device exit $DEV_RC"
expect_line "$T/d4.log" "[UDP] Retransmitting seq=1 (timeout)" TC-SESS-03
n=$(count "$T/s3.log" "HELLO seq=1 received (serial ZTP-55AC90")
[ "$n" -eq 1 ] && ok || bad "TC-SESS-03: $n sessions, want 1"
n=$(count "$T/s3.log" "Retransmitting seq=1")
[ "$n" -eq 0 ] && ok || bad "TC-SESS-03: the server retransmitted HELLO_ACK $n times; the cache should have answered first"
expect_line "$T/d4.log" "[UDP] Session established" TC-SESS-03
stop_server

# --- TC-DISC-04: multicast discovery on loopback ------------------------------------------------
start_server s4
run_device d5 20 --serial ZTP-19FE77 --discover mcast --mcast-iface 127.0.0.1
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-DISC-04: device exit $DEV_RC"
expect_line "$T/d5.log" "[DISCOVERY] Provisioning server found 127.0.0.1:$PORT (OFFER verified)" TC-DISC-04
expect_line "$T/s4.log" "DISCOVER from ZTP-19FE77" TC-DISC-04
stop_server

# --- TC-DISC-06: no discovery socket -> 3 attempts, then static ----------------------------------
start_server s5 --test-hooks --no-discovery
run_device d6 20 --serial ZTP-84BC19 --discover mcast --mcast-iface 127.0.0.1
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-DISC-06: device exit $DEV_RC"
n=$(count "$T/d6.log" "DISCOVER sent to 239.255.90.90:$DPORT")
[ "$n" -eq 3 ] && ok || bad "TC-DISC-06: $n DISCOVER attempts, want 3"
expect_line "$T/d6.log" "[DISCOVERY] Using static server IP 127.0.0.1:$PORT" TC-DISC-06
stop_server

# --- TC-DISC-05: broadcast (only when BCAST is given: Mac Wi-Fi locally, the runner on CI) ------
if [ -n "${BCAST:-}" ]; then
    start_server s6 --set net.mcast_iface_addr=0.0.0.0
    run_device d7 20 --serial ZTP-0B0B0B --discover bcast --bcast-addr "$BCAST"
    expect_line "$T/d7.log" "(OFFER verified)" "TC-DISC-05 ($BCAST)"
    stop_server
fi

# --- UNKNOWN_MODEL: terminal, persisted ---------------------------------------------------------
start_server s7
run_device d8 20 --serial ZTP-999999 --discover static --model EDGE-GW-999
[ "$DEV_RC" -eq 1 ] && ok || bad "UNKNOWN_MODEL: device exit $DEV_RC, want 1"
expect_line "$T/d8.log" "[AUTH] Registration failed: UNKNOWN_MODEL" UNKNOWN_MODEL
expect_line "$T/d8.log" "[STATE] Device is FAILED (reason: UNKNOWN_MODEL)" UNKNOWN_MODEL
grep -q '^stage=FAILED$' "$T/d8.state/device.state" && ok || bad "UNKNOWN_MODEL: stage not persisted"
run_device d8 20 --serial ZTP-999999 --discover static --model EDGE-GW-999
[ "$DEV_RC" -eq 1 ] && ok || bad "UNKNOWN_MODEL restart: exit $DEV_RC, want 1"
run_device d8 20 --serial ZTP-999999 --discover static --model EDGE-GW-999 --reset-state
expect_line "$T/d8.log" "[BOOT] Soft serial assigned (configuration)" "--reset-state"

# --- handshake under loss, reordering and duplication (TC-SIM-02 flavour) ------------------------
stop_server
start_server s8 --loss 0.2 --reorder 0.1 --dup 0.05 --seed 11
for seed in 1 2 3; do
    run_device "dl$seed" 40 --serial "ZTP-00000$seed" --discover static --loss 0.2 --reorder 0.1 --dup 0.05 --seed "$seed"
    [ "$DEV_RC" -eq 0 ] && ok || bad "loss seed $seed: device exit $DEV_RC"
    expect_line "$T/dl$seed.log" "[NAT] Keepalive acknowledged" "loss seed $seed"
done
stop_server
expect_line "$T/s8.log" "[SIM] tx=" "netsim totals"

# --- TC-NAT-11: no server -> blocked-UDP hint after 1 + max_retries HELLO transmissions ---------
run_device d9 6 --serial ZTP-0FF0FF --discover static --set rel.max_retries=2 --set rel.rto_initial_ms=200 --set rel.rto_max_ms=400
expect_line "$T/d9.log" "[NAT] No response from 127.0.0.1:$PORT after 3 HELLO transmissions: check that outbound UDP to port $PORT is allowed (firewall or NAT policy)" TC-NAT-11
expect_line "$T/d9.log" "[UDP] Reconnecting in" "TC-NAT-11 backoff"

# --- TC-REL-02: one data frame lost -> retransmitted once on timeout, delivered once ----------------
start_server s9
run_device d10 20 --serial ZTP-2E1002 --discover static --test-hooks --drop-seq tx:2
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-REL-02: device exit $DEV_RC"
n=$(count "$T/d10.log" "[UDP] Retransmitting seq=2 (timeout)")
[ "$n" -eq 1 ] && ok || bad "TC-REL-02: seq 2 retransmitted $n times, want 1"
stop_server
n=$(count "$T/s9.log" "ZTP-2E1002] Keepalive (path probe) acknowledged")
[ "$n" -eq 1 ] && ok || bad "TC-REL-02: path probe delivered $n times, want 1"
expect_line "$T/d10.log" "[NAT] Keepalive acknowledged" "TC-REL-02 (ACK advances)"

# --- TC-SIM-02: reordering and duplication are applied and visible --------------------------------
start_server s10 --reorder 0.5 --dup 0.5 --seed 21
run_device d11 30 --serial ZTP-51A002 --discover static --reorder 0.5 --dup 0.5 --seed 22
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-SIM-02: device exit $DEV_RC"
stop_server
for f in "$T/s10.log" "$T/d11.log"; do
    line=$(grep -F "[SIM] tx=" "$f" | tail -1)
    re=$(echo "$line" | sed -n 's/.*reordered=\([0-9]*\).*/\1/p'); du=$(echo "$line" | sed -n 's/.*duplicated=\([0-9]*\).*/\1/p')
    [ "${re:-0}" -gt 0 ] && [ "${du:-0}" -gt 0 ] && ok || bad "TC-SIM-02: $(basename "$f"): $line"
done
if grep -F -q "Duplicate packet seq=" "$T/s10.log" "$T/d11.log"; then ok; else bad "TC-SIM-02: no [UDP] duplicate events"; fi

# --- NAT emulator (§9.8) and --rebind-after; the device keeps sending 2 s keepalives after the probe -
HOLD="--set run.operational_hold_s=7"
start_server s12
run_device d12 20 --serial ZTP-AA0001 --discover static $HOLD --nat-sim reboot_at_s=3
[ "$DEV_RC" -eq 0 ] && ok || bad "NAT reboot: device exit $DEV_RC"
expect_line "$T/d12.log" "[SIM] NAT: reboot, all mappings flushed" "NAT reboot"
if grep -q 'ZTP-AA0001\] Peer address changed 127.0.0.1:[0-9]* -> 127.0.0.1:[0-9]* (rebinding)' "$T/s12.log"; then ok; else bad "NAT reboot: no rebinding on the server"; fi

run_device d13 20 --serial ZTP-AA0002 --discover static $HOLD --nat-sim idle_timeout_s=1.5
[ "$DEV_RC" -eq 0 ] && ok || bad "NAT idle: device exit $DEV_RC"
expect_line "$T/d13.log" "[SIM] NAT: mapping expired after idle" "NAT idle timeout"
if grep -q 'ZTP-AA0002\] Peer address changed .* (rebinding)' "$T/s12.log"; then ok; else bad "NAT idle: no rebinding on the server"; fi
if grep -q 'ZTP-AA0002\] Rebinding after [0-9]* s idle: NAT idle timeout is probably below keepalive_interval (2 s); consider lowering it' "$T/s12.log"; then ok
else bad "NAT idle: no rebind advice (§9.7)"; fi

run_device d14 20 --serial ZTP-AA0003 --discover static $HOLD --test-hooks --rebind-after 3
[ "$DEV_RC" -eq 0 ] && ok || bad "--rebind-after: device exit $DEV_RC"
if grep -q 'ZTP-AA0003\] Peer address changed .* (rebinding)' "$T/s12.log"; then ok; else bad "--rebind-after: no rebinding"; fi
n=$(count "$T/s12.log" "HELLO seq=1 received (serial ZTP-AA0003")
[ "$n" -eq 1 ] && ok || bad "--rebind-after: $n HELLOs, want 1 (the session continues)"

if [ "$(uname)" = Linux ]; then   # 127.0.0.2 is local on Linux; macOS needs an lo0 alias
    run_device d15 20 --serial ZTP-AA0004 --discover static $HOLD --nat-sim change_ip_at_s=3
    if grep -q 'ZTP-AA0004\] Peer IP changed 127.0.0.1:[0-9]* -> 127.0.0.2:[0-9]* (allowed)' "$T/s12.log"; then ok
    else bad "NAT IP change: no 'Peer IP changed ... (allowed)'"; fi
fi

# TC-NAT-11 with the server up: a firewall rule drops everything to the server port
run_device d16 6 --serial ZTP-AA0005 --discover static --nat-sim block_server_port \
    --set rel.max_retries=2 --set rel.rto_initial_ms=200 --set rel.rto_max_ms=400
expect_line "$T/d16.log" "after 3 HELLO transmissions: check that outbound UDP to port $PORT is allowed" "TC-NAT-11 (blocked port)"
n=$(count "$T/s12.log" "ZTP-AA0005")
[ "$n" -eq 0 ] && ok || bad "TC-NAT-11: the server saw the blocked device"
stop_server

# --- TC-SESS-08: UDP only ------------------------------------------------------------------------
if grep -rn 'SOCK_STREAM' common server device >/dev/null 2>&1; then bad "TC-SESS-08: SOCK_STREAM in source"; else ok; fi

echo "m2 session tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
