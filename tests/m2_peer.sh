#!/bin/sh
# M2 component and negative tests driven by tests/peer_sim against the real binaries.
# usage: tests/m2_peer.sh path/to/ztp-server path/to/ztp-device path/to/peer_sim
# Env: ZTP_PORT (default 26555; +1 = server discovery port, +2 = a second discovery port).
set -u
SRV=$1
DEV=$2
SIM=$3
PORT=${ZTP_PORT:-26555}
DPORT=$((PORT + 1))
DPORT2=$((PORT + 2))
T=${TMPDIR:-/tmp}/ztp-peer.$$
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
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }
counter() { sed -n "s/.*\[UDP\] Counters: .* $2=\([0-9]*\).*/\1/p" "$1" | tail -1; }   # counter LOG NAME

COMMON="--set net.server_port=$PORT --set net.discovery_port=$DPORT --set net.mcast_iface_addr=127.0.0.1"
start_server() {
    name=$1; shift
    "$SRV" --profile demo $COMMON --set paths.log="$T/$name.log" --set paths.db="$T/$name.db" --set paths.dump="$T/$name.dump" "$@" > "$T/$name.out" 2>&1 &
    SRV_PID=$!
    i=0
    while [ $i -lt 50 ] && ! has "$T/$name.log" "Server listening"; do sleep 0.1; i=$((i + 1)); done
    has "$T/$name.log" "Server listening" || bad "server $name did not start: $(cat "$T/$name.out")"
}
stop_server() {   # SIGTERM, then wait: the exit counters are logged on the way out
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null
    SRV_PID=
}
sim() {           # sim NAME args...: run peer_sim, keep its output, return its status
    name=$1; shift
    "$SIM" --server "127.0.0.1:$PORT" "$@" > "$T/$name.sim" 2>&1
    rc=$?
    sed 's/^/    /' "$T/$name.sim"
    return $rc
}
run_device() {    # run_device NAME SECS args...: foreground with a watchdog; DEV_RC
    name=$1; secs=$2; shift 2
    "$DEV" --profile "${DEV_PROFILE:-demo}" --state-dir "$T/$name.state" --set paths.log="$T/$name.log" \
        --set discovery.server=127.0.0.1:$PORT "$@" > "$T/$name.out" 2>&1 &
    dpid=$!
    ( sleep "$secs"; kill "$dpid" 2>/dev/null ) &
    wpid=$!
    wait "$dpid"
    DEV_RC=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
}
# in_order FILE A B C...: each fixed string appears, in this order
in_order() {
    f=$1; shift
    last=0
    for s in "$@"; do
        n=$(grep -n -F -- "$s" "$f" | head -1 | cut -d: -f1)
        if [ -z "$n" ] || [ "$n" -le "$last" ]; then return 1; fi
        last=$n
    done
    return 0
}

echo "m2 peer_sim tests (ports $PORT/$DPORT/$DPORT2)"

# --- TC-SESS-04: 1,000 handshakes; session ids unique and random -----------------------------
start_server sess04 --set limits.hello_rate_per_ip=100000 --set limits.hello_burst=100000 \
    --set limits.max_sessions=2000 --set limits.max_half_open=2000
sim sess04 handshakes 1000 && ok || bad "TC-SESS-04"
stop_server

# --- TC-SESS-06, TC-REL-03, TC-REL-08, TC-NAT-06/12 against one server -------------------------
start_server s
sim sess06 unknown-sid && ok || bad "TC-SESS-06"
sim rel03 pdf-reorder && ok || bad "TC-REL-03 (peer view)"
sim rel08 out-of-window && ok || bad "TC-REL-08 (peer view)"
sim nat06 spoof-source && ok || bad "TC-NAT-06 / TC-NAT-12 (peer view)"
stop_server
L="$T/s.log"
expect_line "$L" "Resume for an unknown session: SESSION_UNKNOWN" TC-SESS-06
[ "$(counter "$L" rx_unknown)" -ge 1 ] && ok || bad "TC-SESS-06: rx_unknown=$(counter "$L" rx_unknown)"
sid=$(sed -n 's/.*\[sid=\([0-9]*\) ZTP-0D0F03\].*/\1/p' "$L" | head -1)
tag="[UDP] [sid=$sid ZTP-0D0F03]"
if in_order "$L" "$tag Received packet seq=3 (out of order, expected=2)" "$tag Buffered packet seq=3" \
    "$tag Delivered seq=2..3 (drained 1 buffered)" "$tag Duplicate packet seq=2 ignored"; then ok
else bad "TC-REL-03: server lines missing or out of order (sid $sid)"; fi
[ "$(counter "$L" out_of_window)" -eq 1 ] && ok || bad "TC-REL-08: out_of_window=$(counter "$L" out_of_window), want 1"
if grep -q 'ZTP-5900F0\] Peer \(address\|IP\) changed' "$L"; then bad "TC-NAT-06/12: the peer moved"; else ok; fi
[ "$(counter "$L" mac_fail)" -ge 2 ] && ok || bad "TC-NAT-06: mac_fail=$(counter "$L" mac_fail), want >= 2"

# --- TC-SIM-01: loss per direction and end to end, 10k datagrams each way at --loss 0.15 ----------
start_server sim01 --loss 0.15 --seed 5 --set limits.hello_rate_per_ip=1000000 --set limits.hello_burst=1000000
sim sim01 flood-discover 10000 --disc-port "$DPORT" --loss 0.15 --seed 6 || bad "TC-SIM-01: flood failed"
stop_server
# peer_sim: sent=N tx_dropped=D offers_received=R ; server [SIM] tx=Ns dropped=Ds ... rx=Rs
N=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$T/sim01.sim")
D=$(sed -n 's/.*tx_dropped=\([0-9]*\).*/\1/p' "$T/sim01.sim")
R=$(sed -n 's/.*offers_received=\([0-9]*\).*/\1/p' "$T/sim01.sim")
NS=$(sed -n 's/.*\[SIM\] tx=\([0-9]*\).*/\1/p' "$T/sim01.log" | tail -1)
DS=$(sed -n 's/.*\[SIM\] tx=[0-9]* dropped=\([0-9]*\).*/\1/p' "$T/sim01.log" | tail -1)
RS=$(sed -n 's/.*\[SIM\] .* rx=\([0-9]*\).*/\1/p' "$T/sim01.log" | tail -1)
rates=$(awk -v n="$N" -v d="$D" -v ns="$NS" -v ds="$DS" -v rs="$RS" -v r="$R" 'BEGIN {
    printf "%.2f %.2f %.2f %.2f", 100*d/n, 100*ds/ns, 100*(1-rs/n), 100*(1-r/ns) }')
echo "    loss %: peer TX $(echo $rates | cut -d" " -f1), server TX $(echo $rates | cut -d" " -f2), peer->server end to end $(echo $rates | cut -d" " -f3), server->peer end to end $(echo $rates | cut -d" " -f4)"
[ -n "$N" ] && [ -n "$D" ] && [ -n "$R" ] && [ -n "$NS" ] && [ -n "$DS" ] && [ -n "$RS" ] && [ "$(echo $rates | wc -w)" -eq 4 ] && ok ||
    bad "TC-SIM-01: missing measurements (N=$N D=$D R=$R NS=$NS DS=$DS RS=$RS)"
for v in $rates; do
    awk -v x="$v" 'BEGIN { exit !(x >= 13 && x <= 17) }' && ok || bad "TC-SIM-01: a loss rate of $v % is outside 15 +/- 2 % ($rates)"
done

# --- TC-DISC-07: forged OFFERs are ignored, the genuine server is used -------------------------
start_server d07 --test-hooks --no-discovery
"$SIM" rogue-offer --disc-port "$DPORT" --secs 5 > "$T/d07.sim" 2>&1 &
RPID=$!
sleep 0.3
run_device d07 20 --serial ZTP-D15C07 --discover mcast --mcast-iface 127.0.0.1 --set discovery.discovery_port=$DPORT
wait "$RPID" && ok || bad "TC-DISC-07: the rogue answered nothing ($(cat "$T/d07.sim"))"
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-DISC-07: device exit $DEV_RC"
expect_line "$T/d07.log" "[DISCOVERY] OFFER MAC invalid, ignoring" TC-DISC-07
expect_line "$T/d07.log" "[DISCOVERY] Using static server IP 127.0.0.1:$PORT" TC-DISC-07
if has "$T/d07.log" "127.0.0.1:9 "; then bad "TC-DISC-07: the device used the rogue address"; else ok; fi
stop_server

# --- TC-DISC-08: a replayed genuine OFFER (old disc_nonce) is ignored --------------------------
start_server d08
"$SIM" replay-offer --disc-port "$DPORT2" --capture "127.0.0.1:$DPORT" --secs 5 > "$T/d08.sim" 2>&1 &
RPID=$!
sleep 0.5
run_device d08 20 --serial ZTP-D15C08 --discover mcast --mcast-iface 127.0.0.1 --set discovery.discovery_port=$DPORT2
wait "$RPID" && ok || bad "TC-DISC-08: the replayer failed ($(cat "$T/d08.sim"))"
expect_line "$T/d08.log" "[DISCOVERY] OFFER nonce mismatch, ignoring" TC-DISC-08
expect_line "$T/d08.log" "[DISCOVERY] Using static server IP 127.0.0.1:$PORT" TC-DISC-08
stop_server

# --- TC-CFG-08: the device clamps advertised values to its own bounds ---------------------------
# The bounds come from a config file, not --set: a device-side --set of nat.keepalive_interval_s
# deliberately overrides whatever the server advertises (DESIGN §25.3), which would hide the clamp.
cat > "$T/cfg08.json" <<'JSON'
{ "nat": { "keepalive_min_s": 5, "keepalive_interval_s": 20, "keepalive_max_s": 300 },
  "run": { "exit_when_operational": true, "operational_hold_s": 1 },
  "net": { "reconnect_min_s": 0.5, "reconnect_max_s": 2 } }
JSON
for case in "1 50 5 10" "900 3 300 3"; do
    set -- $case
    adv_k=$1; adv_d=$2; want_k=$3; want_d=$4
    "$SIM" fake-server --listen "127.0.0.1:$PORT" --keepalive "$adv_k" --dead "$adv_d" --secs 8 > "$T/cfg08.sim" 2>&1 &
    FPID=$!
    sleep 0.3
    DEV_PROFILE=default run_device "cfg08_$adv_k" 20 --serial ZTP-C0F608 --discover static -v --config "$T/cfg08.json"
    kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null
    expect_line "$T/cfg08_$adv_k.log" "[NAT] Advertised keepalive $adv_k s clamped to $want_k s" "TC-CFG-08 ($adv_k s)"
    [ "$adv_d" -ne "$want_d" ] && expect_line "$T/cfg08_$adv_k.log" "Advertised dead_intervals $adv_d clamped to $want_d" "TC-CFG-08 (dead $adv_d)"
    expect_line "$T/cfg08_$adv_k.log" "keepalive $want_k s, dead after $want_d intervals" "TC-CFG-08 (effective values)"
done

echo "m2 peer_sim tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
