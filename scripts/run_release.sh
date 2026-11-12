#!/bin/sh
# The release regression (QA §11.2, M7): every suite, the sanitizer build, the whole impairment matrix, the
# performance and soak runs, the long NAT runs, and the committed demo and Appendix A logs, all recorded in one
# results folder. `make release`; about 2.5 hours (the soak is 1 h, the long NAT runs 30 min).
# usage: scripts/run_release.sh   (from the repository root)
# Env: RESULTS (default results/<date-time>-release), SEEDS (matrix seeds, default 10), SOAK_S (default 3600),
#      QUICK=1 (skip the soak and the long NAT runs)
set -u
cd "$(dirname "$0")/.." || exit 1
RESULTS=${RESULTS:-results/$(date +%Y%m%d-%H%M%S)-release}
export RESULTS
SEEDS=${SEEDS:-10}
SUITE="sh tests/run_suite.sh"
fail=0
step() { "$@" || fail=$((fail + 1)); }

echo "release regression: $RESULTS"
step make --no-print-directory test
step make --no-print-directory asan
step $SUITE matrix env OUT="$RESULTS/run_matrix" ROWS="M1 M2 M3 M4 M5 M8 M9" SEEDS="$SEEDS" sh scripts/run_matrix.sh
step $SUITE perf02 env OUT="$RESULTS/perf02" ROWS=M4 SEEDS=20 sh scripts/run_matrix.sh
step $SUITE scale env OUT="$RESULTS/run_scale" sh scripts/run_scale.sh 50
if [ "${QUICK:-0}" != 1 ]; then
    step $SUITE soak env OUT="$RESULTS/run_soak" sh scripts/run_soak.sh
    step $SUITE longrun sh tests/m5_longrun.sh build/ztp-server build/ztp-device build/mkartifacts
fi
step make --no-print-directory demo
step make --no-print-directory appendix-a
echo
cat "$RESULTS/summary.txt"
[ -f "$RESULTS/asan/summary.txt" ] && { echo; echo "ASan build:"; tail -n +7 "$RESULTS/asan/summary.txt"; }
echo
[ "$fail" -eq 0 ] && echo "release regression PASS ($RESULTS)" || echo "release regression: $fail step(s) FAILED ($RESULTS)"
[ "$fail" -eq 0 ]
