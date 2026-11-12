#!/bin/sh
# Appendix A reference run (QA TC-APX-01..10, TC-PDF-04): one device, ZTP-93AF21 (EDGE-GW-100, v1.0), against a
# fresh server whose first session id is 7421, checked step by step in the order the PDF shows. Then the same
# run under impairment (15% loss, 10% reorder, 5% duplication) with a fresh server and state per seed.
# usage: tests/check_appendix_a.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts [ADDR [PORT]]
# Env: APX_SEEDS (default 10; 0 skips TC-APX-10), APX_NO_STEPDOWN=1 (TC-PROV-12: no §5.8 step-down), APX_PCAP (a live capture of the reference run, written by
# `tcpdump -U`: checks TC-APX-02/03 on the wire; needs pcapcheck at build/pcapcheck).
# The literal nonce=984312 of Step 5 needs the --nonce hook, which is open tooling (QA §12.5): the REGISTER
# line is checked up to "nonce=".
set -u
SRV=$1
DEV=$2
MKA=$3
ADDR=${4:-127.0.0.1}
PORT=${5:-36555}
SEEDS=${APX_SEEDS:-10}
T=${TMPDIR:-/tmp}/ztp-apx.$$
mkdir -p "$T/art"
pass=0
fail=0
SRV_PID=
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    if [ "$fail" -eq 0 ]; then rm -rf "$T"; else echo "  (logs kept in $T)"; fi
}
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
A="$T/art"
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": { "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$A/fw_EDGE-GW-100_v1.1.bin", "config_version": 3,
                               "config_file": "$A/cfg_v3.json", "tenant": "cruise_ops" } } }
JSON

# The PDF's steps, in order (a line may be followed by others: retransmissions, duplicates, progress)
cat > "$T/steps" <<STEPS
1|[BOOT] Device starting
1|[BOOT] Soft serial: ZTP-93AF21
1|[BOOT] Firmware version: v1.0
2|[DISCOVERY] Using static server IP $ADDR:$PORT
3|[UDP] HELLO sent (seq=1)
3|[UDP] Session established (session_id=7421)
4|[NAT] Keepalive sent
4|[NAT] Keepalive acknowledged
5|[AUTH] REGISTER: session_id=7421 soft_serial=ZTP-93AF21 model=EDGE-GW-100 firmware=v1.0 nonce=
5|[AUTH] Registration successful
5|[AUTH] Assigned tenant: cruise_ops
6|[PROVISION] Receiving config (chunk 1/3)
6|[PROVISION] Receiving config (chunk 2/3)
6|[PROVISION] Receiving config (chunk 3/3)
6|[PROVISION] Config checksum verified
7|[UPGRADE] Current: v1.0 -> Target: v1.1
7|[UPGRADE] Upgrade complete, restarting device
8|[BOOT] Restart detected
8|[UDP] Session resumed (session_id=7421)
9|[VALIDATION] Configuration present
9|[VALIDATION] Firmware v1.1 verified
9|[STATE] Device is OPERATIONAL
STEPS

# run_apx NAME [impairment args...]: a fresh server and device; checks every step; TC is the label prefix
run_apx() {
    name=$1; shift
    "$SRV" --profile demo --test-hooks --first-session-id 7421 --set net.bind="$ADDR" --set net.server_port="$PORT" \
        --set net.discovery_port=$((PORT + 1)) --policy "$T/policy.json" --set paths.log="$T/$name.s.log" \
        --set paths.db="$T/$name.db" --set paths.dump="$T/$name.dump" --dump-on-exit --dump-verbose ${SRV_IMP:-} \
        > "$T/$name.s.out" 2>&1 &
    SRV_PID=$!
    i=0
    while [ $i -lt 80 ] && ! has "$T/$name.s.log" "Server listening"; do sleep 0.1; i=$((i + 1)); done
    "$DEV" --profile demo --state-dir "$T/$name.state" --set paths.log="$T/$name.d.log" --serial ZTP-93AF21 \
        --model EDGE-GW-100 --fw v1.0 --discover static --server "$ADDR:$PORT" "$@" > "$T/$name.d.out" 2>&1 &
    dpid=$!
    ( sleep 90; kill "$dpid" 2>/dev/null ) &
    wpid=$!
    wait "$dpid"
    rc=$?
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=
    mv "$T/db_dump_verbose.txt" "$T/$name.verbose" 2>/dev/null
    f="$T/$name.d.log"
    [ "$rc" -eq 0 ] && ok || bad "$name: device exit $rc"
    # every step line, in order
    if out=$(awk -F'|' -v lf="$f" '
        { step[NR] = $1; want[NR] = substr($0, index($0, "|") + 1); n = NR }
        END {
            k = 1
            while ((getline l < lf) > 0 && k <= n) if (index(l, want[k])) k++
            if (k <= n) { printf "TC-APX-%02d: \"%s\" missing or out of order", step[k], want[k]; exit 1 }
        }' "$T/steps"); then ok; else bad "$name $out"; fi
    awk '/Restart detected/ { a = 1 } a && /\[AUTH\] REGISTER:/ { f = 1 } END { exit f }' "$f" \
        && ok || bad "$name TC-APX-08: REGISTER after the restart"
    [ "$(grep -c 'ZTP-93AF21\] Registration successful' "$T/$name.s.log")" -eq 1 ] && ok \
        || bad "$name TC-APX-05/08: server registrations for ZTP-93AF21 != 1"
    row=$(grep '| ZTP-93AF21 ' "$T/$name.verbose" 2>/dev/null)
    echo "$row" | grep -q 'Success' && ok || bad "$name TC-APX-05: DB row not Success"
    echo "$row" | grep -q 'OPERATIONAL' && ok || bad "$name TC-APX-09: DB stage not OPERATIONAL"
    [ "$(grep -c '| ZTP-93AF21 ' "$T/$name.verbose" 2>/dev/null)" -eq 1 ] && ok || bad "$name TC-APX-08: not exactly one DB row"
    v=$(dd if="$T/$name.state/fw_slot_B.bin" bs=1 skip=23 count=8 2>/dev/null | tr -d '\000')
    [ "$v" = v1.1 ] && ok || bad "$name TC-APX-07: slot B header version '$v'"
}

echo "Appendix A reference run ($ADDR:$PORT)"
SRV_IMP=
run_apx ref
if [ -n "${APX_PCAP:-}" ]; then   # TC-APX-02/03 on the wire
    PC=$(dirname "$0")/../build/pcapcheck
    "$PC" --dump-hdr "$APX_PCAP" "$PORT" > "$T/hdr.txt" 2>&1
    grep -q "^$ADDR:$PORT\$\|-> $ADDR:$PORT " "$T/hdr.txt" && ok || bad "TC-APX-02: no datagram to $ADDR:$PORT in the capture"
    grep -q "-> $ADDR:$PORT HELLO .*sid=0 .*seq=1 " "$T/hdr.txt" && ok || bad "TC-APX-03: no HELLO seq=1 to the server"
    grep -q "^$ADDR:$PORT -> .* HELLO_ACK .*sid=7421 .*seq=1 " "$T/hdr.txt" && ok || bad "TC-APX-03: no HELLO_ACK seq=1 sid=7421"
    LC_ALL=C grep -a -q 'ZTP-93AF21' "$APX_PCAP" && ok || bad "TC-APX-03: SERIAL TLV not in the capture"
fi
if [ "$ADDR" = 10.10.10.5 ] && [ "$PORT" = 5555 ]; then   # TC-PDF-04: the reference run on the PDF's address
    grep -q 'Provisioning server found 10.10.10.5:5555 (static)' "$T/ref.d.log" && ok || bad "TC-PDF-04: server not found at 10.10.10.5:5555"
fi

if [ "${APX_NO_STEPDOWN:-0}" = 1 ]; then   # TC-PROV-12: a 1280-byte MTU path needs no fallback
    cat "$T/ref.d.log" "$T/ref.s.log" | grep -q 'datagram limit now' && bad "TC-PROV-12: the datagram limit stepped down" || ok
fi

# TC-APX-10: the same under impairment, a fresh server and state per seed
s=1
while [ "$s" -le "$SEEDS" ]; do
    SRV_IMP="--loss 0.15 --reorder 0.10 --dup 0.05 --seed $s"
    run_apx "imp$s" --loss 0.15 --reorder 0.10 --dup 0.05 --seed $((s + 100))
    s=$((s + 1))
done

echo "Appendix A checks: $pass passed, $fail failed (Step 5 nonce value not checked: --nonce is open tooling, QA §12.5)"
[ "$fail" -eq 0 ]
