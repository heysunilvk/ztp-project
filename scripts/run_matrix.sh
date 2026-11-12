#!/bin/sh
# The impairment matrix (QA §6, TC-REL-09; TC-E2E-02 is row M3; TC-PERF-02 is row M4 with 20 seeds): the
# 5-device demo, a fresh server and state per run, for each row and seed. Per row: the runs that passed, the
# valid devices that reached OPERATIONAL, and the time from first boot to OPERATIONAL (median and p95).
# usage: scripts/run_matrix.sh   (from the repository root, after `make`)
# Env: ROWS="M2 M3 M4" (any of M1 M2 M3 M4 M5 M8 M9), SEEDS=10 (seeds 1..SEEDS), PORT=42555, BIN=build,
#      OUT=<temp dir> (kept on failure), MEDIAN_MAX/P95_MAX (seconds, checked for row M4: 60/90, SM-1)
set -u
cd "$(dirname "$0")/.." || exit 1
ROWS=${ROWS:-M2 M3 M4}
SEEDS=${SEEDS:-10}
PORT=${PORT:-42555}
BIN=${BIN:-build}
OUT=${OUT:-${TMPDIR:-/tmp}/ztp-matrix.$$}
MEDIAN_MAX=${MEDIAN_MAX:-60}
P95_MAX=${P95_MAX:-90}
mkdir -p "$OUT"

row_args() {   # the QA §6 row as "SERVER_ARGS|DEVICE_ARGS" (loss is per direction, at the sender)
    case $1 in
    M1) echo "|" ;;
    M2) a="--loss 0.10 --reorder 0.05 --dup 0.02 --delay 1"; echo "$a|$a" ;;
    M3) a="--loss 0.15 --reorder 0.10 --dup 0.05 --delay 2 --jitter 1"; echo "$a|$a" ;;
    M4) a="--loss 0.20 --reorder 0.10 --dup 0.05 --delay 5 --jitter 2"; echo "$a|$a" ;;
    M5) a="--loss 0.30 --reorder 0.20 --dup 0.10 --delay 5 --jitter 2"; echo "$a|$a" ;;
    M8) echo "|--loss 0.20" ;;   # device -> server only
    M9) echo "--loss 0.20|" ;;   # server -> device only
    *) echo "?" ;;
    esac
}

overall=0
printf '%-4s %-7s %-13s %-9s %-7s %s\n' row runs operational median p95 result
for r in $ROWS; do
    a=$(row_args "$r")
    [ "$a" = "?" ] && { echo "run_matrix: unknown row $r"; exit 2; }
    sa=${a%%|*}; da=${a#*|}
    okruns=0; s=1
    while [ "$s" -le "$SEEDS" ]; do
        d="$OUT/$r.$s"
        mkdir -p "$d"
        if SRV_IMPAIR="$sa" DEV_IMPAIR="$da" SEED="$s" DEMO_DIR="$d" PORT="$PORT" BIN="$BIN" QUIET=1 \
            sh scripts/run_demo.sh > "$d/demo.txt" 2>&1; then okruns=$((okruns + 1)); fi
        s=$((s + 1))
    done
    # every valid device's time to OPERATIONAL (its log's first line to its last OPERATIONAL line)
    stats=$(python3 -I - "$OUT" "$r" <<'PY'
import datetime, glob, sys, statistics
out, row = sys.argv[1], sys.argv[2]
def ts(l): return datetime.datetime.strptime(l[:23], "%Y-%m-%dT%H:%M:%S.%f")
times, total, opn = [], 0, 0
for f in sorted(glob.glob("%s/%s.*/logs/device_*.log" % (out, row))):
    lines = [l for l in open(f, errors="replace") if l[:2] == "20"]
    if not lines: continue
    total += 1
    op = [l for l in lines if "[STATE] Device is OPERATIONAL" in l]
    if op:
        opn += 1
        times.append((ts(op[-1]) - ts(lines[0])).total_seconds())
times.sort()
med = statistics.median(times) if times else float("nan")
p95 = times[min(len(times) - 1, int(round(0.95 * (len(times) - 1))))] if times else float("nan")
print("%d %d %.1f %.1f" % (opn, total, med, p95))
PY
)
    set -- $stats
    opn=$1; total=$2; med=$3; p95=$4
    res=PASS
    if [ "$r" = M5 ]; then   # stress row: >= 95% of the valid devices
        [ $((opn * 100)) -ge $((total * 95)) ] || res=FAIL
    else
        [ "$okruns" -eq "$SEEDS" ] || res=FAIL
    fi
    if [ "$r" = M4 ]; then
        python3 -I -c "import sys; sys.exit(0 if $med <= $MEDIAN_MAX and $p95 <= $P95_MAX else 1)" || res="FAIL (time)"
    fi
    [ "$res" = PASS ] || overall=1
    printf '%-4s %-7s %-13s %-9s %-7s %s\n' "$r" "$okruns/$SEEDS" "$opn/$total" "${med}s" "${p95}s" "$res"
done
if [ "$overall" -eq 0 ]; then echo "matrix PASS"; rm -rf "$OUT"
else
    echo "matrix FAIL (runs kept in $OUT; each holds demo.txt and logs/)"
    grep -l 'demo FAIL' "$OUT"/*/demo.txt 2>/dev/null | head -5 | while read -r f; do echo "--- $f"; grep 'FAIL' "$f" | head -5; done
fi
exit $overall
