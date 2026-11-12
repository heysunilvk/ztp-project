#!/bin/sh
# Runs one regression suite and records it (QA §11 M7 row, item 6). With RESULTS set (`make test` sets
# RESULTS=results/<date-time>), the suite's output goes to $RESULTS/<name>.out as well as the terminal, the
# suite's own logs and state stay in $RESULTS/<name>/, and one line is appended to $RESULTS/summary.txt.
# Without RESULTS it just runs the command.
# usage: tests/run_suite.sh NAME COMMAND [ARGS...]
set -u
name=$1
shift
[ -n "${RESULTS:-}" ] || exec "$@"
mkdir -p "$RESULTS"
sum="$RESULTS/summary.txt"
if [ ! -f "$sum" ]; then
    {
        echo "ZTP regression run $(basename "$RESULTS")"
        echo "started:  $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "commit:   $(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet HEAD 2>/dev/null || echo ' (+ local changes)')"
        echo "host:     $(uname -sm), $(${CC:-cc} --version 2>/dev/null | head -1)"
        echo "build:    ${BUILD:-build}"
        echo
        printf '%-16s %-6s %6s  %s\n' suite result time output
    } > "$sum"
fi
start=$(date +%s)
{ "$@" 2>&1; echo $? > "$RESULTS/.$name.rc"; } | tee "$RESULTS/$name.out"
rc=$(cat "$RESULTS/.$name.rc" 2>/dev/null || echo 1)
rm -f "$RESULTS/.$name.rc"
dur=$(( $(date +%s) - start ))
res=PASS; [ "$rc" -eq 0 ] || res=FAIL
last=$(grep -v '^ *$' "$RESULTS/$name.out" | tail -1)
printf '%-16s %-6s %5ss  %s\n' "$name" "$res" "$dur" "$last" >> "$sum"
[ "$rc" -eq 0 ] || echo "  ($name failed: output in $RESULTS/$name.out, logs in $RESULTS/$name/)"
exit "$rc"
