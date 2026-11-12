#!/bin/sh
# Log checks for a demo or scale run (QA TC-LOG-01..04).
# usage: tests/check_logs.sh LOGDIR   (LOGDIR holds server.log and device_<serial>.log)
set -u
L=$1
pass=0
fail=0
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
SRVL="$L/server.log"
[ -f "$SRVL" ] || { echo "check_logs: no $SRVL"; exit 1; }
set -- "$L"/device_*.log
[ -f "$1" ] || { echo "check_logs: no device logs in $L"; exit 1; }
NDEV=$#

# TC-LOG-01: every required tag in at least one device log, and the server-side ones in the server log
for tag in BOOT DISCOVERY UDP NAT AUTH PROVISION UPGRADE VALIDATION STATE; do
    cat "$@" | grep -q "Z \[$tag\] " && ok || bad "TC-LOG-01: no [$tag] in any device log"
done
# the server logs [DISCOVERY] only when a device discovered it (a static-only run has none)
stags="BOOT UDP NAT AUTH PROVISION UPGRADE STATE"
cat "$@" | grep -q '(OFFER verified)' && stags="$stags DISCOVERY"
for tag in $stags; do
    grep -q "Z \[$tag\] " "$SRVL" && ok || bad "TC-LOG-01: no [$tag] in the server log"
done

# TC-LOG-02: the PDF's bonus lines, generic sequence numbers (any log of the run). They need loss, reordering or
# duplication, so an unimpaired run (QA §6 row M1) sets CHECK_BONUS=0, and a loss-only run (rows M8, M9)
# CHECK_BONUS=retx: only the retransmission line is certain there.
case "${CHECK_BONUS:-1}" in
1)    bonus_res='Received packet seq=[0-9]+ \(out of order, expected=[0-9]+\)|Buffered packet seq=[0-9]+|Retransmitting seq=[0-9]+ \(timeout\)|Duplicate packet seq=[0-9]+ ignored' ;;
retx) bonus_res='Retransmitting seq=[0-9]+ \(timeout\)' ;;
*)    bonus_res= ;;
esac
[ -n "$bonus_res" ] && { IFS='|'; set -f; for re in $bonus_res; do unset IFS;
    cat "$@" "$SRVL" | grep -Eq "$re" && ok || bad "TC-LOG-02: no line matching '$re'"
done; unset IFS; set +f; }

# TC-LOG-03: ISO-8601 UTC timestamps with milliseconds, never going backwards within a file. Device logs are
# all log lines; the server log also carries the dump table and summary, which are not log lines.
ts='^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z '
for f in "$@" "$SRVL"; do
    b=$(basename "$f")
    if [ "$f" != "$SRVL" ]; then
        n=$(grep -Evc "$ts" "$f"); [ "$n" -eq 0 ] && ok || bad "TC-LOG-03: $b has $n lines without a timestamp"
    fi
    grep -E "$ts" "$f" | cut -c1-23 | awk 'prev > $0 { bad = NR } { prev = $0 } END { exit bad > 0 }' \
        && ok || bad "TC-LOG-03: $b timestamps go backwards"
done
# one file per device; a device's log continues across its restart (boot #2 in the same file)
nout=$(ls "$L"/device_*.out 2>/dev/null | wc -l | tr -d ' ')
[ "$nout" = 0 ] || [ "$nout" -eq "$NDEV" ] && ok || bad "TC-LOG-03: $nout devices ran but $NDEV device logs"
for f in "$@"; do
    if grep -q 'Restart detected' "$f"; then
        grep -q 'Device starting (boot #1)' "$f" && grep -q 'Device starting (boot #2)' "$f" && ok \
            || bad "TC-LOG-03: $(basename "$f") does not hold both boots"
    fi
done

# TC-LOG-04: the counter summary at exit, on both sides
grep -q '\[UDP\] Counters: tx=' "$SRVL" && ok || bad "TC-LOG-04: no counters in the server log"
for f in "$@"; do
    grep -q '\[UDP\] Counters: tx=' "$f" && ok || bad "TC-LOG-04: no counters in $(basename "$f")"
done

echo "check_logs: $pass passed, $fail failed ($NDEV device logs)"
[ "$fail" -eq 0 ]
