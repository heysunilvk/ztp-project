#!/bin/sh
# The 5-device demo (DESIGN §19.3, PRD §5.2 scenarios S1-S5, QA §4.1): one server and five devices, run
# concurrently over an impaired network, then the server's dump and every device log are checked.
#
# usage: scripts/run_demo.sh            (from the repository root, after `make`)
# Env:
#   SEED=42          impairment seed (each device uses SEED + its index)
#   IMPAIR="--loss 0.15 --reorder 0.10 --dup 0.05 --delay 2 --jitter 1"   (QA §6 row M3); IMPAIR= for none
#   SRV_IMPAIR, DEV_IMPAIR   per side, overriding IMPAIR (loss is applied at the sender: rows M8/M9)
#   DEMO_DIR=.       where logs/ and state/ go (tests use a temp directory); artifacts/ stay in the repo
#   PORT=5555        server port (+1 = discovery)
#   BIN=build        where ztp-server, ztp-device and mkartifacts are
#   TIMEOUT=240      seconds to wait for the devices
#   QUIET=1          only the PASS/FAIL summary
# Exit status: 0 PASS, 1 FAIL.
set -u
cd "$(dirname "$0")/.." || exit 1
SEED=${SEED:-42}
IMPAIR=${IMPAIR-"--loss 0.15 --reorder 0.10 --dup 0.05 --delay 2 --jitter 1"}
SRV_IMPAIR=${SRV_IMPAIR-$IMPAIR}
DEV_IMPAIR=${DEV_IMPAIR-$IMPAIR}
DEMO_DIR=${DEMO_DIR:-.}
PORT=${PORT:-5555}
BIN=${BIN:-build}
TIMEOUT=${TIMEOUT:-240}
QUIET=${QUIET:-0}
L="$DEMO_DIR/logs"
S="$DEMO_DIR/state"
SRV="$BIN/ztp-server"
DEV="$BIN/ztp-device"
say() { [ "$QUIET" = 1 ] || echo "$@"; }

for b in "$SRV" "$DEV" "$BIN/mkartifacts"; do [ -x "$b" ] || { echo "run_demo: $b not built (run make)"; exit 1; }; done
# Scenarios whose hooks the device doesn't have are skipped, and reported (PRD R-15, QA §12.5)
HELP=$("$DEV" --help 2>&1)
HAVE_BAD_KEY=0; echo "$HELP" | grep -q -- '--bad-key' && HAVE_BAD_KEY=1
HAVE_REPLAY=0; echo "$HELP" | grep -q -- '--replay-test' && HAVE_REPLAY=1

# 1. clean state and logs; artifacts
rm -rf "$L" "$S"
mkdir -p "$L" "$S"
"$BIN/mkartifacts" --out artifacts > /dev/null || { echo "run_demo: mkartifacts failed"; exit 1; }

# 2. the server (impairment row M3; the demo profile's short timers)
SRV_LOG="$L/server.log"
"$SRV" --profile demo --test-hooks --dump-verbose --set net.server_port="$PORT" --set net.discovery_port=$((PORT + 1)) \
    --set net.mcast_iface_addr=127.0.0.1 --set paths.log="$SRV_LOG" --set paths.db="$S/db.jsonl" \
    --set paths.dump="$L/db_dump.txt" $SRV_IMPAIR --seed "$SEED" > "$L/server.out" 2>&1 &
SRV_PID=$!
i=0
while [ $i -lt 100 ] && ! grep -q 'Server listening' "$SRV_LOG" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
grep -q 'Server listening' "$SRV_LOG" 2>/dev/null || { echo "run_demo: the server did not start:"; cat "$L/server.out"; exit 1; }

# 3. the five devices, concurrently. A device that crashes (abort, the S3 hook) or exits 75 is relaunched with
# the same command line, the way a supervisor would (at most 3 times).
dev() {   # dev INDEX SERIAL ARGS...: runs one device under its supervisor; writes its final exit code
    idx=$1; serial=$2; shift 2
    exec 2>> "$L/device_$serial.out"   # the shell's own "Abort trap" notice for the S3 crash goes here too
    n=0
    while :; do
        "$DEV" --profile demo --state-dir "$S/$serial" --set paths.log="$L/device_$serial.log" --serial "$serial" \
            --set discovery.discovery_port=$((PORT + 1)) $DEV_IMPAIR --seed $((SEED + idx)) "$@" >> "$L/device_$serial.out" 2>&1
        rc=$?
        n=$((n + 1))
        if { [ $rc -eq 75 ] || [ $rc -ge 128 ]; } && [ $n -le 3 ]; then
            echo "supervisor: $serial exited $rc, relaunching" >> "$L/device_$serial.out"
            continue
        fi
        echo "$rc" > "$S/$serial.rc"
        echo "$n" > "$S/$serial.runs"
        return
    done
}
STATIC="--discover static --server 127.0.0.1:$PORT"
dev 1 ZTP-93AF21 --model EDGE-GW-100 --fw v1.0 --discover mcast --mcast-iface 127.0.0.1 &
P1=$!
if [ $HAVE_BAD_KEY = 1 ]; then dev 2 ZTP-84BC19 --model EDGE-GW-100 --fw v1.0 $STATIC --test-hooks --bad-key & P2=$!; else P2=; fi
REPLAY=; [ $HAVE_REPLAY = 1 ] && REPLAY=--replay-test
dev 3 ZTP-71DD08 --model EDGE-GW-100 --fw v1.0 $STATIC --test-hooks --crash-after-chunks 40 $REPLAY &
P3=$!
# S4: the server moves the peer only on a new frame, never on a duplicate (DESIGN §9.4), so the rebinding shows
# only once a new frame from the new port gets through. A 15 s hold leaves several keepalive rounds after the
# rebind at 3 s; with the default 5 s hold, a lossy seed could exit first (M3 seeds 4 and 10, M9 seed 1).
dev 4 ZTP-55AC90 --model EDGE-GW-100 --fw v1.1 $STATIC --test-hooks --rebind-after 3 --set run.operational_hold_s=15 &
P4=$!
dev 5 ZTP-19FE77 --model EDGE-GW-050 --fw v0.9 $STATIC &
P5=$!
say "demo: server pid $SRV_PID, seed $SEED, impairment: server ${SRV_IMPAIR:-none}; devices ${DEV_IMPAIR:-none}"

# TC-SRV-04: a dump on demand while the devices are provisioning
( sleep 4; kill -USR1 "$SRV_PID" 2>/dev/null ) &
USR1=$!

# 4. wait for every device (with a timeout), then SIGINT: the dump and summary
( sleep "$TIMEOUT"; pkill -f "state-dir $S/" 2>/dev/null ) > /dev/null 2>&1 &
WD=$!
wait $P1 ${P2:-} $P3 $P4 $P5
kill "$WD" 2>/dev/null
wait "$WD" 2>/dev/null
wait "$USR1" 2>/dev/null
kill -INT "$SRV_PID"
wait "$SRV_PID"

# 5. the checks
pass=0; fail=0; skipped=""
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
skip() { skipped="$skipped$1; "; }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }
row() { grep "^| $1 " "$L/db_dump.txt" | head -1; }
vrow() { grep "^| $1 " "$L/db_dump_verbose.txt" | head -1; }
cell() { echo "$1" | awk -F'|' -v n="$2" '{ gsub(/^ +| +$/, "", $(n + 1)); print $(n + 1) }'; }
rc_of() { cat "$S/$1.rc" 2>/dev/null || echo x; }
expect_row() {   # expect_row SERIAL STATUS FW CFG NOTES TC
    r=$(row "$1")
    [ -n "$r" ] || { bad "$6: no dump row for $1"; return; }
    got="$(cell "$r" 2)|$(cell "$r" 3)|$(cell "$r" 4)|$(cell "$r" 6)"
    [ "$got" = "$2|$3|$4|$5" ] && ok || bad "$6: $1 row is '$got', want '$2|$3|$4|$5'"
}

# S1 ZTP-93AF21: multicast discovery, upgrade, restart, resume, OPERATIONAL
d="$L/device_ZTP-93AF21.log"
[ "$(rc_of ZTP-93AF21)" = 0 ] && ok || bad "S1: ZTP-93AF21 exit $(rc_of ZTP-93AF21)"
for l in "(OFFER verified)" "[UPGRADE] Upgrade complete, restarting device" "[BOOT] Restart detected" "[UDP] Session resumed (session_id=" "[STATE] Device is OPERATIONAL"; do
    has "$d" "$l" && ok || bad "S1: missing \"$l\""
done
expect_row ZTP-93AF21 Success v1.1 v3 "—" "S1/TC-PDF-02"

# S2 ZTP-84BC19: three verified Invalid HMAC rejections, FAILED
if [ $HAVE_BAD_KEY = 1 ]; then
    [ "$(rc_of ZTP-84BC19)" = 1 ] && ok || bad "S2: ZTP-84BC19 exit $(rc_of ZTP-84BC19), want 1 (FAILED)"
    has "$L/device_ZTP-84BC19.log" "Registration failed: Invalid HMAC (3/3), giving up" && ok || bad "S2: no third Invalid HMAC"
    expect_row ZTP-84BC19 Failed "—" "—" "Invalid HMAC" "S2/TC-PDF-02"
else
    skip "S2 ZTP-84BC19 (needs --bad-key)"
fi

# S3 ZTP-71DD08: abort at chunk 40, relaunched, only the missing chunks re-requested, Success
d="$L/device_ZTP-71DD08.log"
[ "$(rc_of ZTP-71DD08)" = 0 ] && ok || bad "S3: ZTP-71DD08 exit $(rc_of ZTP-71DD08)"
[ "$(cat "$S/ZTP-71DD08.runs" 2>/dev/null)" = 2 ] && ok || bad "S3: want 2 runs (crash, relaunch), got $(cat "$S/ZTP-71DD08.runs" 2>/dev/null)"
has "$d" "TEST HOOK FIRED: crash-after-chunks" && ok || bad "S3: the crash hook did not fire"
if grep -q 'Resuming firmware: [1-9][0-9]*/64 chunks present, requesting [0-9]*' "$d"; then
    req=$(sed -n 's/.*Resuming firmware: [0-9]*\/64 chunks present, requesting \([0-9]*\).*/\1/p' "$d" | head -1)
    [ "$req" -lt 64 ] && ok || bad "S3: re-requested $req of 64 chunks"
else bad "S3: the download was not resumed"; fi
[ "$(grep -c '\[AUTH\] REGISTER:' "$d")" -eq 1 ] && ok || bad "S3: registered more than once"
expect_row ZTP-71DD08 Success v1.1 v3 "—" "S3/TC-PDF-02"
vrow ZTP-71DD08 | grep -q 'Transfer resumed' && ok || bad "S3: no \"Transfer resumed\" note in the verbose dump"
[ $HAVE_REPLAY = 1 ] || skip "S3 REGISTER replays (needs --replay-test)"

# S4 ZTP-55AC90: already current, survives a NAT rebinding
d="$L/device_ZTP-55AC90.log"
[ "$(rc_of ZTP-55AC90)" = 0 ] && ok || bad "S4: ZTP-55AC90 exit $(rc_of ZTP-55AC90)"
has "$d" "Firmware v1.1 is current, no upgrade required" && ok || bad "S4: not reported current"
has "$d" "Upgrade complete" && bad "S4: upgraded although current" || ok
# the rebinding (--rebind-after 3) normally lands in a registered session, which the server moves to the new
# address; under heavy loss it can land before registration, and then the device's fresh session simply starts
# from the new address (one REGISTER either way)
if awk '/--rebind-after: new source address/ { r = NR } /\[AUTH\] Registration successful/ && !g { g = NR } END { exit !(g && r && g < r) }' "$d"; then
    grep -q 'ZTP-55AC90\] Peer address changed .* (rebinding)' "$SRV_LOG" && ok || bad "S4: the server saw no rebinding"
else
    [ "$(grep -c '\[AUTH\] REGISTER:' "$d")" -eq 1 ] && ok || bad "S4: rebinding before registration, but more than one REGISTER"
fi
expect_row ZTP-55AC90 Success v1.1 v3 "—" "S4/TC-PDF-02"

# S5 ZTP-19FE77: legacy model, static discovery
[ "$(rc_of ZTP-19FE77)" = 0 ] && ok || bad "S5: ZTP-19FE77 exit $(rc_of ZTP-19FE77)"
has "$L/device_ZTP-19FE77.log" "[UPGRADE] Current: v0.9 -> Target: v1.0" && ok || bad "S5: no upgrade v0.9 -> v1.0"
expect_row ZTP-19FE77 Success v1.0 v2 "Legacy device" "S5/TC-PDF-02"

# TC-PDF-02: the dump's format (the PDF's §9 example)
D="$L/db_dump.txt"
has "$D" "Device Registration Details" && ok || bad "TC-PDF-02: no title"
hdr=$(grep '^| Device ID' "$D" | head -1 | tr -s ' ' | sed 's/ |/|/g; s/| /|/g')
[ "$hdr" = "|Device ID|Registration Status|Firmware Assigned|Config Version|Registration Time|Notes|" ] && ok \
    || bad "TC-PDF-02: header is '$hdr'"
times_ok=1
for sn in ZTP-93AF21 ZTP-71DD08 ZTP-55AC90 ZTP-19FE77; do
    t=$(cell "$(row "$sn")" 5)
    echo "$t" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$' || times_ok=0
done
[ $times_ok = 1 ] && ok || bad "TC-PDF-02: a Registration Time is not YYYY-MM-DD HH:MM:SS"
# --dump-verbose adds columns after Notes only: the verbose header starts with the default one (TC-SRV-04)
vh=$(grep '^| Device ID' "$L/db_dump_verbose.txt" | head -1 | tr -s ' ' | sed 's/ |/|/g; s/| /|/g')
case "$vh" in "|Device ID|Registration Status|Firmware Assigned|Config Version|Registration Time|Notes|"?*) ok ;;
    *) bad "TC-SRV-04: the verbose dump does not extend the default columns after Notes" ;; esac
# TC-UPG-04: Attempts = 1 for every device that registered (only the bad-key one may have more); the verbose
# dump's columns are found by name
att=$(awk -F'|' 'NR == 1 || !/^\| (Device ID|ZTP-)/ { next }
    /^\| Device ID/ { for (i = 2; i < NF; i++) { h = $i; gsub(/^ +| +$/, "", h); if (h == "Attempts") c = i }; next }
    c { s = $2; a = $c; gsub(/ /, "", s); gsub(/ /, "", a); if (s != "ZTP-84BC19" && a != "1") print s "=" a }' "$L/db_dump_verbose.txt")
[ -z "$att" ] && ok || bad "TC-UPG-04: Attempts not 1 for: $att"
# TC-UPG-04 / invariant: exactly one row per serial
dupes=$(grep '^| ZTP-' "$D" | awk -F'|' '{ print $2 }' | sort | uniq -d)
[ -z "$dupes" ] && ok || bad "TC-UPG-04: duplicate rows for $dupes"

# TC-SRV-04: the dump was printed twice (SIGUSR1 mid-run, SIGINT at the end)
[ "$(grep -c 'Device Registration Details' "$L/server.out")" -ge 2 ] && ok || bad "TC-SRV-04: the SIGUSR1 dump was not printed"
# TC-SRV-05: the summary
nok=4; nfail=0; [ $HAVE_BAD_KEY = 1 ] && nfail=1
grep -q "Registered successfully : $nok" "$D" && ok || bad "TC-SRV-05: summary does not list $nok successes"
[ $HAVE_BAD_KEY = 1 ] || skip "TC-SRV-05 failure count (needs S2)"

# TC-SRV-09: provisioning from at least 3 sessions interleaves in the server log. Every session line from the
# first to the last [PROVISION] line counts: the [PROVISION] lines alone are a few per session, and on a fast
# run they could come out one session after another while the sessions overlapped (CI macOS, M2 seed 1)
sids=$(awk '/\[PROVISION\] \[sid=/ { if (!f) f = NR; l = NR } { L[NR] = $0 }
            END { for (i = f; i <= l; i++) if (L[i] ~ /\[sid=[0-9]+ /) print L[i] }' "$SRV_LOG" |
       sed -n 's/.*\[sid=\([0-9]*\) .*/\1/p')
nd=$(echo "$sids" | sort -u | grep -c .)
switches=$(echo "$sids" | awk 'prev != "" && $0 != prev { n++ } { prev = $0 } END { print n + 0 }')
[ "$nd" -ge 3 ] && [ "$switches" -ge "$nd" ] && ok || bad "TC-SRV-09: $nd sessions, $switches switches: not interleaved"

# TC-LOG-01..04 and the invariants (shared with the scale test)
# TC-LOG-02 bonus lines: retransmissions need loss; out-of-order and buffered lines are certain only with
# reordering (a loss-only row such as M8 or M9 can finish a seed without a gap in the sequence)
bonus=1
case "$SRV_IMPAIR $DEV_IMPAIR" in *--reorder*) ;; *) bonus=retx ;; esac
[ -z "$SRV_IMPAIR$DEV_IMPAIR" ] && bonus=0   # no impairment, none of them
CHECK_BONUS=$bonus sh tests/check_logs.sh "$L" > "$L/check_logs.txt" 2>&1 && ok || { bad "check_logs.sh"; cat "$L/check_logs.txt"; }
sh tests/check_invariants.sh "$L" "$S" artifacts > "$L/check_invariants.txt" 2>&1 && ok || { bad "check_invariants.sh"; cat "$L/check_invariants.txt"; }

# TC-SRV-06: a restarted server rebuilds the same rows from the journal
"$SRV" --profile demo --set net.server_port="$PORT" --set net.discovery_port=$((PORT + 1)) --set paths.log="$L/server_restart.log" \
    --set paths.db="$S/db.jsonl" --set paths.dump="$L/db_dump_restart.txt" --run-for 1 --dump-on-exit > /dev/null 2>&1
if [ "$(grep '^| ZTP-' "$D")" = "$(grep '^| ZTP-' "$L/db_dump_restart.txt" 2>/dev/null)" ]; then ok
else bad "TC-SRV-06: the rows differ after a server restart"; fi

[ "$QUIET" = 1 ] || { echo; cat "$D"; echo; }
if [ -n "$skipped" ]; then echo "demo: skipped (open tooling, QA §12.5): $skipped"; fi
if [ "$fail" -eq 0 ]; then echo "demo PASS ($pass checks, seed $SEED)"; exit 0; fi
echo "demo FAIL ($fail of $((pass + fail)) checks, seed $SEED; logs in $L)"
exit 1
