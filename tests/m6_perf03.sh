#!/bin/sh
# TC-PERF-03: retransmission overhead against the model of DESIGN §21.1 Q5. A frame needs its datagram and
# the ACK to get through, so at loss p per direction the expected transmissions per reliable frame are at most
# 1/(1-p)^2 (1.23, 1.56, 2.04 at 10/20/30%); the measurement must stay within that bound + 10%.
# One upgrading device per run (loss only: the model has no reorder or duplication); the frames and
# retransmissions of both sides, from the exit counters (the device logs one line per process).
# usage: tests/m6_perf03.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts
# Env: PERF_SEEDS (default 3), ZTP_PORT (default 46555)
set -u
SRV=$1
DEV=$2
MKA=$3
SEEDS=${PERF_SEEDS:-3}
PORT=${ZTP_PORT:-46555}
T=${SUITE_DIR:-${RESULTS:+$RESULTS/m6_perf03}}; T=${T:-${TMPDIR:-/tmp}/ztp-perf03.$$}   # kept under RESULTS (make test)
mkdir -p "$T/art"
pass=0
fail=0
cleanup() { if [ "$fail" -ne 0 ]; then echo "  (logs kept in $T)"; elif [ -z "${RESULTS:-}" ]; then rm -rf "$T"; fi; }
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": { "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$T/art/fw_EDGE-GW-100_v1.1.bin", "config_version": 3,
                               "config_file": "$T/art/cfg_v3.json", "tenant": "cruise_ops" } } }
JSON

echo "m6 TC-PERF-03 ($SEEDS seeds per loss rate)"
printf '  %-5s %-8s %-8s %-8s %-7s %s\n' loss frames sent ratio bound result
for p in 0.10 0.20 0.30; do
    s=1
    while [ "$s" -le "$SEEDS" ]; do
        n="p$p.s$s"
        "$SRV" --profile demo --set net.server_port=$PORT --set net.discovery_port=$((PORT + 1)) --policy "$T/policy.json" \
            --set paths.log="$T/$n.s.log" --set paths.db="$T/$n.db" --set paths.dump="$T/$n.dump" --loss "$p" --seed "$s" \
            > "$T/$n.s.out" 2>&1 &
        sp=$!
        i=0
        while [ $i -lt 80 ] && ! grep -q 'Server listening' "$T/$n.s.log" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
        "$DEV" --profile demo --state-dir "$T/$n.state" --set paths.log="$T/$n.d.log" --set run.operational_hold_s=1 \
            --serial ZTP-93AF21 --model EDGE-GW-100 --fw v1.0 --discover static --server 127.0.0.1:$PORT \
            --loss "$p" --seed $((s + 100)) > "$T/$n.d.out" 2>&1 &
        dp=$!
        ( sleep 120; kill "$dp" 2>/dev/null ) > /dev/null 2>&1 &
        wp=$!
        wait "$dp" 2>/dev/null
        kill "$wp" 2>/dev/null; wait "$wp" 2>/dev/null
        kill -INT "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
        grep -q '\[STATE\] Device is OPERATIONAL' "$T/$n.d.log" || bad "loss $p seed $s: the device did not reach OPERATIONAL"
        s=$((s + 1))
    done
    # sum frames and retransmissions over every Counters line of this loss rate (both sides, all processes)
    set -- $(cat "$T"/p$p.s*.s.log "$T"/p$p.s*.d.log | grep '\[UDP\] Counters: tx=' | awk '
        { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "frames") f += kv[2];
              if (kv[1] == "retx_timeout" || kv[1] == "retx_fast") r += kv[2] } }
        END { printf "%d %d\n", f, f + r }')
    frames=$1; sent=$2
    res=$(python3 -I -c "
p = $p; f = $frames; s = $sent
bound = 1 / (1 - p) ** 2
ratio = s / f if f else float('inf')
print('%.2f %.2f %s' % (ratio, bound, 'PASS' if f and ratio <= bound * 1.10 else 'FAIL'))")
    set -- $res
    printf '  %-5s %-8s %-8s %-8s %-7s %s\n' "$p" "$frames" "$sent" "$1" "$2" "$3"
    [ "$3" = PASS ] && ok || bad "loss $p: $1 transmissions per frame, bound $2 + 10%"
done
echo "m6 TC-PERF-03: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
