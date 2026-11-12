#!/bin/sh
# Long-run keepalive tests under 20% loss per direction (QA TC-NAT-07, TC-NAT-13). Too long for every push:
# run with `make longrun` (both cases in parallel, about 31 min).
# usage: tests/m5_longrun.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts
# Env: ZTP_PORT (default 33555; the second case uses +10), NAT07_S (default 1800), NAT13_S (default 600).
set -u
SRV=$1
DEV=$2
MKA=$3
PORT=${ZTP_PORT:-33555}
NAT07_S=${NAT07_S:-1800}
NAT13_S=${NAT13_S:-600}
T=${RESULTS:+$RESULTS/m5_longrun}; T=${T:-${TMPDIR:-/tmp}/ztp-m5l.$$}   # kept under RESULTS (make test)
mkdir -p "$T/art"
pass=0
fail=0
PIDS=
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    if [ "$fail" -ne 0 ]; then echo "  (logs kept in $T)"; elif [ -z "${RESULTS:-}" ]; then rm -rf "$T"; fi
}
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
count() { n=$(grep -F -c -- "$2" "$1" 2>/dev/null); echo "${n:-0}"; }

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
A="$T/art"
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": { "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$A/fw_EDGE-GW-100_v1.1.bin", "config_version": 3,
                               "config_file": "$A/cfg_v3.json", "tenant": "cruise_ops" } } }
JSON

# start_case NAME PORT SEED PROFILE_ARGS...: a server and an already-current device, both at 20% loss
start_case() {
    name=$1; p=$2; seed=$3; shift 3
    "$SRV" "$@" --set net.server_port=$p --set net.discovery_port=$((p + 1)) --policy "$T/policy.json" \
        --set paths.log="$T/$name.s.log" --set paths.db="$T/$name.db" --set paths.dump="$T/$name.dump" \
        --loss 0.2 --seed "$seed" > "$T/$name.s.out" 2>&1 &
    PIDS="$PIDS $!"
    sleep 1
    "$DEV" "$@" --state-dir "$T/$name.state" --set paths.log="$T/$name.d.log" --set run.exit_when_operational=false \
        --discover static --server 127.0.0.1:$p --serial ZTP-7A000$seed --fw v1.1 --loss 0.2 --seed $((seed + 100)) \
        > "$T/$name.d.out" 2>&1 &
    PIDS="$PIDS $!"
}

echo "m5 long-run tests: TC-NAT-07 ${NAT07_S} s (default profile), TC-NAT-13 ${NAT13_S} s (demo profile), 20% loss"
start_case nat07 "$PORT" 1
start_case nat13 $((PORT + 10)) 2 --profile demo
sleep "$NAT13_S"
# stop TC-NAT-13 (its two processes are the last two started)
set -- $PIDS
kill "$3" "$4" 2>/dev/null
[ "$NAT07_S" -gt "$NAT13_S" ] && sleep $((NAT07_S - NAT13_S))
kill "$1" "$2" 2>/dev/null
sleep 1

# TC-NAT-07: production timers (keepalive 20 s, dead after 3 intervals): no session timeout in 30 min
# Only the OPERATIONAL part counts: setup under 20% loss may legitimately exhaust a retry budget and start over
op() { awk '/\[STATE\] Device is OPERATIONAL/ { on = 1 } on' "$1" > "$1.op"; echo "$1.op"; }
grep -q '\[STATE\] Device is OPERATIONAL' "$T/nat07.d.log" && ok || bad "TC-NAT-07: never OPERATIONAL"
f=$(op "$T/nat07.d.log")
to=$(count "$f" "Session timeout detected"); ka=$(count "$f" "[NAT] Keepalive sent")
[ "$to" -eq 0 ] && ok || bad "TC-NAT-07: $to session timeouts (want 0)"
echo "  TC-NAT-07: $ka keepalives while OPERATIONAL, $to session timeouts, $(count "$T/nat07.d.log" "[AUTH] REGISTER:") REGISTER"

# TC-NAT-13: demo timers (2 s / 6 s): every timeout recovers by a resume within reconnect_max_s (5 s) + an RTT;
# never a re-registration
grep -q '\[STATE\] Device is OPERATIONAL' "$T/nat13.d.log" && ok || bad "TC-NAT-13: never OPERATIONAL"
f=$(op "$T/nat13.d.log")
[ "$(count "$f" "[AUTH] REGISTER:")" -eq 0 ] && ok || bad "TC-NAT-13: registered again while OPERATIONAL"
# each "Session timeout detected" must be followed by "Session resumed" within 6 s (timestamps are ISO-8601 UTC)
if python3 -I - "$f" <<'PY'
import sys, datetime
def ts(l): return datetime.datetime.strptime(l[:23], "%Y-%m-%dT%H:%M:%S.%f")
pending, worst, n, bad = None, 0.0, 0, 0
lines = open(sys.argv[1]).read().splitlines()
for l in lines:
    if "Session timeout detected" in l:
        if pending is not None: bad += 1
        pending = ts(l); n += 1
    elif "Session resumed" in l and pending is not None:
        d = (ts(l) - pending).total_seconds(); worst = max(worst, d); pending = None
        if d > 6.0: bad += 1
if pending is not None and (ts(lines[-1]) - pending).total_seconds() > 6.0: bad += 1
ka = sum("Keepalive sent" in l for l in lines)
print("  TC-NAT-13: %d keepalives, %d session timeouts, slowest recovery %.1f s" % (ka, n, worst))
sys.exit(1 if bad else 0)
PY
then ok; else bad "TC-NAT-13: a timeout without a resume within 6 s"; fi

echo "m5 long-run tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
