#!/bin/sh
# Invariants of every demo, matrix and scale run (QA §6): zero duplicate DB rows, zero REGISTERs after a
# restart, and every artifact a device kept verified: config.json and the active firmware slot are
# byte-identical to the server's artifacts.
# usage: tests/check_invariants.sh LOGDIR STATEDIR ARTIFACTDIR
#   LOGDIR: db_dump.txt and device_<serial>.log; STATEDIR: one state directory per serial
set -u
L=$1
S=$2
A=$3
pass=0
fail=0
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
sha() {   # SHA-256 of a file; a missing file or tool gives a unique non-hash, so two of them never compare equal
    if command -v shasum > /dev/null 2>&1; then h=$(shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1)
    else h=$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1); fi
    echo "${h:-no-hash:$1}"
}
val() { sed -n "s/^$2=//p" "$1" 2>/dev/null | head -1; }

D="$L/db_dump.txt"
[ -f "$D" ] || { echo "check_invariants: no $D"; exit 1; }
dupes=$(grep '^| ZTP-' "$D" | awk -F'|' '{ gsub(/ /, "", $2); print $2 }' | sort | uniq -d | tr '\n' ' ')
[ -z "$dupes" ] && ok || bad "duplicate DB rows: $dupes"

n=0
for f in "$L"/device_*.log; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    serial=$(basename "$f" .log | sed 's/^device_//')
    # no REGISTER after any restart: the device resumed instead (§12.3)
    awk '/Restart detected/ { r = 1 } r && /\[AUTH\] REGISTER:/ { bad = 1 } END { exit bad }' "$f" \
        && ok || bad "$serial: REGISTER after a restart"
    st="$S/$serial/device.state"
    [ "$(val "$st" stage)" = OPERATIONAL ] || continue   # only what a device accepted is checked
    model=$(val "$st" model)
    # the config kept is the one the server serves for the assigned version
    cfgv=$(val "$st" cfg_version)
    if [ -f "$A/cfg_v$cfgv.json" ]; then
        [ "$(sha "$S/$serial/config.json")" = "$(sha "$A/cfg_v$cfgv.json")" ] && ok || bad "$serial: config.json differs from cfg_v$cfgv.json"
    fi
    # an upgraded device runs exactly the target image
    slot=$(val "$st" active_slot)
    if [ -f "$S/$serial/fw_slot_$slot.bin" ]; then
        fw=$(val "$st" fw_target)
        art="$A/fw_${model}_$fw.bin"
        [ -f "$art" ] && [ "$(sha "$S/$serial/fw_slot_$slot.bin")" = "$(sha "$art")" ] && ok \
            || bad "$serial: slot $slot differs from $(basename "$art")"
    fi
done
[ "$n" -gt 0 ] && ok || bad "no device logs in $L"
echo "check_invariants: $pass passed, $fail failed ($n devices)"
[ "$fail" -eq 0 ]
