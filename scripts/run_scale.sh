#!/bin/sh
# Scale test (QA TC-PERF-01): N devices provision concurrently against one server at 15% loss (QA §6 row M3).
# Pass: every valid device OPERATIONAL, all done within LIMIT_S, the server's peak RSS within RSS_MB, and the
# invariants and log checks of the demo.
# usage: scripts/run_scale.sh [N]   (from the repository root, after `make`; `make scale N=50`)
# Env: N (default 50), IMPAIR (default row M3), SEED (default 1), PORT (default 45555), BIN (default build),
#      OUT (default a temp dir, kept on failure), LIMIT_S (default 120), RSS_MB (default 64)
set -u
cd "$(dirname "$0")/.." || exit 1
N=${1:-${N:-50}}
IMPAIR=${IMPAIR-"--loss 0.15 --reorder 0.10 --dup 0.05 --delay 2 --jitter 1"}
SEED=${SEED:-1}
PORT=${PORT:-45555}
BIN=${BIN:-build}
OUT=${OUT:-${RESULTS:+$RESULTS/run_scale}}; OUT=${OUT:-${TMPDIR:-/tmp}/ztp-scale.$$}   # kept under RESULTS
LIMIT_S=${LIMIT_S:-120}
RSS_MB=${RSS_MB:-64}
L="$OUT/logs"
S="$OUT/state"
mkdir -p "$L" "$S"
"$BIN/mkartifacts" --out artifacts > /dev/null || { echo "run_scale: mkartifacts failed"; exit 1; }

# peak RSS: BSD time -l reports bytes, GNU time -v kilobytes
if /usr/bin/time -l true > /dev/null 2>&1; then TIMEF=-l; else TIMEF=-v; fi
/usr/bin/time $TIMEF "$BIN/ztp-server" --profile demo --set net.server_port="$PORT" --set net.discovery_port=$((PORT + 1)) \
    --set paths.log="$L/server.log" --set paths.db="$S/db.jsonl" --set paths.dump="$L/db_dump.txt" \
    $IMPAIR --seed "$SEED" > "$L/server.out" 2> "$L/server.time" &
SRV_PID=$!   # the time wrapper: SIGINT reaches the server through it
i=0
while [ $i -lt 100 ] && ! grep -q 'Server listening' "$L/server.log" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
grep -q 'Server listening' "$L/server.log" 2>/dev/null || { echo "run_scale: the server did not start"; cat "$L/server.out"; exit 1; }

# N devices: 4 in 5 are EDGE-GW-100 on v1.0 (upgrade to v1.1), 1 in 5 EDGE-GW-050 on v0.9 (legacy, v1.0)
start=$(date +%s)
pids=
k=1
while [ "$k" -le "$N" ]; do
    serial=$(printf 'ZTP-5C%04X' "$k")
    if [ $((k % 5)) -eq 0 ]; then mf="--model EDGE-GW-050 --fw v0.9"; else mf="--model EDGE-GW-100 --fw v1.0"; fi
    "$BIN/ztp-device" --profile demo --state-dir "$S/$serial" --set paths.log="$L/device_$serial.log" --serial "$serial" \
        $mf --discover static --server "127.0.0.1:$PORT" --set discovery.discovery_port=$((PORT + 1)) \
        $IMPAIR --seed $((SEED * 1000 + k)) > "$L/device_$serial.out" 2>&1 &
    pids="$pids $!"
    k=$((k + 1))
done
( sleep $((LIMIT_S + 60)); for p in $pids; do kill "$p" 2>/dev/null; done ) > /dev/null 2>&1 &
WD=$!
for p in $pids; do wait "$p" 2>/dev/null; done
end=$(date +%s)
kill "$WD" 2>/dev/null; wait "$WD" 2>/dev/null
kill -INT "$SRV_PID" 2>/dev/null
sleep 0.2
pkill -INT -f "paths.log=$L/server.log" 2>/dev/null
wait "$SRV_PID" 2>/dev/null

pass=0; fail=0
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
opn=$(grep -l '\[STATE\] Device is OPERATIONAL' "$L"/device_*.log 2>/dev/null | wc -l | tr -d ' ')
el=$((end - start))
if [ "$TIMEF" = -l ]; then rss=$(awk '/maximum resident set size/ { print int($1 / 1048576) }' "$L/server.time")
else rss=$(awk -F: '/Maximum resident set size/ { print int($2 / 1024) }' "$L/server.time"); fi
[ "$opn" -eq "$N" ] && ok || bad "$opn of $N devices OPERATIONAL"
[ "$el" -le "$LIMIT_S" ] && ok || bad "all done in $el s (limit $LIMIT_S s)"
[ -n "$rss" ] && [ "$rss" -le "$RSS_MB" ] && ok || bad "server peak RSS ${rss:-?} MB (limit $RSS_MB MB)"
[ "$(grep -c '^| ZTP-5C' "$L/db_dump.txt" 2>/dev/null)" -eq "$N" ] && ok || bad "the dump does not hold $N rows"
sh tests/check_invariants.sh "$L" "$S" artifacts > "$L/check_invariants.txt" 2>&1 && ok || { bad "check_invariants.sh"; tail -5 "$L/check_invariants.txt"; }
sh tests/check_logs.sh "$L" > "$L/check_logs.txt" 2>&1 && ok || { bad "check_logs.sh"; grep FAIL "$L/check_logs.txt" | head -5; }
peak=$(sed -n 's/.*sessions_peak=\([0-9]*\).*/\1/p' "$L/server.log" | tail -1)
echo "scale: $N devices, $opn OPERATIONAL in ${el} s, server peak RSS ${rss:-?} MB, peak ${peak:-?} sessions"
if [ "$fail" -eq 0 ]; then echo "scale PASS"; [ -n "${RESULTS:-}" ] || rm -rf "$OUT"; exit 0; fi
echo "scale FAIL (logs in $L)"
exit 1
