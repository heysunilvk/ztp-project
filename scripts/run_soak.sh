#!/bin/sh
# Soak test (QA TC-PERF-04): 5 OPERATIONAL devices keep their sessions alive against one server for SOAK_S
# seconds (1 h by default; demo timers, so a keepalive every 2 s). Pass: every process still running, the
# resident size of each at the end no more than 5% above its value after the warm-up, and no leaks: `leaks` on macOS
# (on the live processes), valgrind on Linux (VALGRIND=1: the server and one device run under memcheck).
# usage: scripts/run_soak.sh   (`make soak`)
# Env: SOAK_S (default 3600), WARMUP_S (default 60), PORT (default 47555), BIN (default build), OUT, VALGRIND
set -u
cd "$(dirname "$0")/.." || exit 1
SOAK_S=${SOAK_S:-3600}
WARMUP_S=${WARMUP_S:-60}
PORT=${PORT:-47555}
BIN=${BIN:-build}
OUT=${OUT:-${TMPDIR:-/tmp}/ztp-soak.$$}
VALGRIND=${VALGRIND:-0}
L="$OUT/logs"
S="$OUT/state"
mkdir -p "$L" "$S"
"$BIN/mkartifacts" --out artifacts > /dev/null || exit 1
VG=
[ "$VALGRIND" = 1 ] && VG="valgrind --quiet --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --error-exitcode=42"

$VG "$BIN/ztp-server" --profile demo --set net.server_port="$PORT" --set net.discovery_port=$((PORT + 1)) \
    --set paths.log="$L/server.log" --set paths.db="$S/db.jsonl" --set paths.dump="$L/db_dump.txt" \
    > "$L/server.out" 2>&1 &
SP=$!
i=0
while [ $i -lt 300 ] && ! grep -q 'Server listening' "$L/server.log" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
pids=
k=1
for serial in ZTP-5D0001 ZTP-5D0002 ZTP-5D0003 ZTP-5D0004 ZTP-5D0005; do
    pre=; [ "$k" = 1 ] && pre=$VG   # under valgrind: the server and the first device
    $pre "$BIN/ztp-device" --profile demo --state-dir "$S/$serial" --set paths.log="$L/device_$serial.log" --serial "$serial" \
        --model EDGE-GW-100 --fw v1.1 --discover static --server "127.0.0.1:$PORT" --set run.exit_when_operational=false \
        > "$L/device_$serial.out" 2>&1 &
    pids="$pids $!"
    k=$((k + 1))
done
rss() { ps -o rss= -p "$1" 2>/dev/null | tr -d ' '; }
sleep "$WARMUP_S"
pass=0; fail=0
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
opn=$(grep -l '\[STATE\] Device is OPERATIONAL' "$L"/device_*.log 2>/dev/null | wc -l | tr -d ' ')
[ "$opn" -eq 5 ] && ok || bad "$opn of 5 devices OPERATIONAL after the warm-up"
before=
for p in $SP $pids; do before="$before $(rss "$p")"; done
echo "soak: warm-up done (${WARMUP_S} s), RSS (kB):$before; soaking ${SOAK_S} s"
sleep "$SOAK_S"
after=
for p in $SP $pids; do after="$after $(rss "$p")"; done
echo "soak: RSS (kB) at the end:$after"
set -- $before
idx=0
for a in $after; do
    b=$1; shift
    name=server; [ "$idx" -gt 0 ] && name="device $idx"
    if [ -z "$a" ] || [ -z "$b" ]; then bad "$name is no longer running"
    elif [ "$VALGRIND" = 1 ]; then ok   # memcheck's own allocations make RSS meaningless; leaks are checked below
    # growth of at most 5% (256 kB of slack for page granularity). A smaller RSS is not a fault: macOS
    # compresses and reclaims an idle process's pages, so resident size falls over a soak; a leak grows it.
    elif [ $((a * 100)) -le $((b * 105 + 25600)) ]; then ok
    else bad "$name RSS $b kB -> $a kB (grew more than 5%)"; fi
    idx=$((idx + 1))
done
# leaks in the live processes (macOS)
if [ "$VALGRIND" != 1 ] && command -v leaks > /dev/null 2>&1; then
    for p in $SP $pids; do
        # `leaks` itself sometimes cannot read a process's malloc zones ("unable to instantiate a memory
        # scanner"): retry, and report a scan that never ran as inconclusive, not as a pass or a leak
        try=1
        while :; do
            leaks "$p" > "$L/leaks.$p.txt" 2>&1
            grep -q 'leaks for .* total leaked bytes' "$L/leaks.$p.txt" && break
            [ $try -ge 3 ] && break
            try=$((try + 1)); sleep 2
        done
        if grep -q ' 0 leaks for 0 total leaked bytes' "$L/leaks.$p.txt"; then ok
        elif grep -q 'leaks for .* total leaked bytes' "$L/leaks.$p.txt"; then bad "leaks in pid $p: $(grep -m1 'leaks for' "$L/leaks.$p.txt")"
        else bad "leaks could not scan pid $p after 3 tries (inconclusive): $(grep -m1 'fatal' "$L/leaks.$p.txt")"; fi
    done
fi
for p in $pids; do kill -INT "$p" 2>/dev/null; done
first=1
for p in $pids; do
    wait "$p" 2>/dev/null
    rc=$?
    # memcheck exits 42 on any error or definite/indirect leak (--error-exitcode)
    if [ "$VALGRIND" = 1 ] && [ $first = 1 ]; then [ $rc -ne 42 ] && ok || bad "valgrind: errors in the device (see device_ZTP-5D0001.out)"; fi
    first=0
done
kill -INT "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
src=$?
if [ "$VALGRIND" = 1 ]; then
    [ $src -ne 42 ] && ok || bad "valgrind: errors in the server (see server.out)"
    for f in "$L/server.out" "$L/device_ZTP-5D0001.out"; do
        grep -q 'definitely lost: [1-9]\|indirectly lost: [1-9]\|Invalid read\|Invalid write\|uninitialised' "$f" \
            && bad "valgrind: $(basename "$f"): $(grep -m1 'lost: [1-9]\|Invalid\|uninitialised' "$f")" || ok
    done
fi
to=$(cat "$L"/device_*.log | grep -c 'Session timeout detected')
echo "soak: $(cat "$L"/device_*.log | grep -c 'Keepalive sent') keepalives, $to session timeouts"
[ "$to" -eq 0 ] && ok || bad "$to session timeouts on a lossless link"
if [ "$fail" -eq 0 ]; then echo "soak PASS"; rm -rf "$OUT"; exit 0; fi
echo "soak FAIL (logs in $L)"
exit 1
