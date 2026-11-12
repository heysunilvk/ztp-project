#!/bin/sh
# M4 integration tests: config and firmware transfer, integrity, resume, the small-MTU fallback.
# usage: tests/m4_provision.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts
# Env: ZTP_PORT (default 29555; +1 = discovery).
set -u
SRV=$1
DEV=$2
MKA=$3
PORT=${ZTP_PORT:-29555}
DPORT=$((PORT + 1))
T=${SUITE_DIR:-${RESULTS:+$RESULTS/m4_provision}}; T=${T:-${TMPDIR:-/tmp}/ztp-m4.$$}   # kept under RESULTS (make test)
mkdir -p "$T/art"
pass=0
fail=0
SRV_PID=
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    if [ "$fail" -ne 0 ]; then echo "  (logs kept in $T)"; elif [ -z "${RESULTS:-}" ]; then rm -rf "$T"; fi
}
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }
count() { grep -F -c -- "$2" "$1" 2>/dev/null || true; }
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }
sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }

# Artifacts: the standard set plus the QA §4.2 boundary files, all copies under $T (tests modify some)
"$MKA" --out "$T/art" > /dev/null && "$MKA" --test-set "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
# firmware images need a valid header (model and the target version, §12.1); fw_tiny (1 B) can't hold one
for f in "fw_1023 v2.2 1023" "fw_1024 v2.3 1024" "fw_1025 v2.4 1025" "fw_1m v2.5 1048576" "fw_corrupt v3.0 65536" "fw_resume v4.0 1048576"; do
    set -- $f
    "$MKA" --fw "$T/art/$1" EDGE-GW-100 "$2" "$3" > /dev/null || { echo "mkartifacts --fw failed"; exit 1; }
done
A="$T/art"
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": {
    "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$A/fw_EDGE-GW-100_v1.1.bin", "config_version": 3,
                     "config_file": "$A/cfg_v3.json", "tenant": "cruise_ops" },
    "EDGE-GW-050": { "firmware": "v1.0", "firmware_file": "$A/fw_EDGE-GW-050_v1.0.bin", "config_version": 2,
                     "config_file": "$A/cfg_v2.json", "tenant": "cruise_ops", "notes": "Legacy device" } },
  "serials": {
    "ZTP-B00001": { "firmware": "v2.1", "firmware_file": "$A/fw_tiny" },
    "ZTP-B00002": { "firmware": "v2.2", "firmware_file": "$A/fw_1023" },
    "ZTP-B00003": { "firmware": "v2.3", "firmware_file": "$A/fw_1024" },
    "ZTP-B00004": { "firmware": "v2.4", "firmware_file": "$A/fw_1025" },
    "ZTP-B00005": { "firmware": "v2.5", "firmware_file": "$A/fw_1m" },
    "ZTP-BAD007": { "firmware": "v3.0", "firmware_file": "$A/fw_corrupt" },
    "ZTP-2E5000": { "firmware": "v4.0", "firmware_file": "$A/fw_resume" } } }
JSON

start_server() {   # start_server NAME [args...]
    name=$1; shift
    "$SRV" --profile demo --set net.server_port=$PORT --set net.discovery_port=$DPORT --policy "$T/policy.json" \
        --set paths.log="$T/$name.log" --set paths.db="$T/$name.db" --set paths.dump="$T/$name.dump" "$@" \
        > "$T/$name.out" 2>&1 &
    SRV_PID=$!
    i=0
    while [ $i -lt 80 ] && ! has "$T/$name.log" "Server listening"; do sleep 0.1; i=$((i + 1)); done
    has "$T/$name.log" "Server listening" || bad "server $name did not start: $(cat "$T/$name.out")"
}
stop_server() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null; SRV_PID=; }
start_device() {   # start_device NAME [args...]: background, sets DPID
    name=$1; shift
    "$DEV" --profile demo --state-dir "$T/$name.state" --set paths.log="$T/$name.log" --set run.operational_hold_s=1 \
        --set discovery.discovery_port=$DPORT --discover static --server 127.0.0.1:$PORT "$@" > "$T/$name.out" 2>&1 &
    DPID=$!
}
run_device() {     # run_device NAME SECS [args...]: foreground with a watchdog; DEV_RC
    name=$1; secs=$2; shift 2
    start_device "$name" "$@"
    ( sleep "$secs"; kill "$DPID" 2>/dev/null ) &
    wpid=$!
    wait "$DPID"
    DEV_RC=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
}
wait_line() {      # wait_line FILE TEXT SECS
    i=0
    while [ $i -lt $(($3 * 20)) ] && ! has "$1" "$2"; do sleep 0.05; i=$((i + 1)); done
    has "$1" "$2"
}

echo "m4 provisioning tests (ports $PORT/$DPORT)"

# --- TC-PROV-01/02/03, TC-REL-06: config (3 chunks) and firmware (64 chunks), hashes, window -----
start_server s1
run_device d1 30 --serial ZTP-93AF21
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-PROV-01: device exit $DEV_RC"
for i in 1 2 3; do expect_line "$T/d1.log" "[PROVISION] Receiving config (chunk $i/3)" TC-PROV-01; done
expect_line "$T/d1.log" "[PROVISION] Config checksum verified" TC-PROV-01
python3 -m json.tool "$T/d1.state/config.json" > /dev/null 2>&1 && ok || bad "TC-PROV-02: config.json is not valid JSON"
grep -q '"config_version": 3' "$T/d1.state/config.json" && grep -q '"keepalive_interval": 20' "$T/d1.state/config.json" && ok ||
    bad "TC-PROV-02: config keys"
expect_line "$T/d1.log" "[PROVISION] Firmware manifest: v1.1, 65536 bytes, 64 chunks" TC-PROV-03
expect_line "$T/d1.log" "[PROVISION] Firmware checksum verified" TC-PROV-03
[ "$(sha "$T/d1.state/fw_slot_B.bin")" = "$(sha "$A/fw_EDGE-GW-100_v1.1.bin")" ] && ok || bad "TC-PROV-03: slot sha differs"
[ "$(sha "$T/d1.state/config.json")" = "$(sha "$A/cfg_v3.json")" ] && ok || bad "TC-PROV-01: config sha differs"
grep -q '^stage=OPERATIONAL$' "$T/d1.state/device.state" && ok || bad "TC-PROV-03: stage"
ls "$T/d1.state"/xfer_* > /dev/null 2>&1 && bad "TC-PROV-03: transfer files left behind" || ok
w=$(sed -n 's/.*ZTP-93AF21\] Firmware v1.1 delivered and verified by the device (max in flight \([0-9]*\)).*/\1/p' "$T/s1.log")
[ -n "$w" ] && [ "$w" -le 8 ] && [ "$w" -ge 2 ] && ok || bad "TC-REL-06: max in flight '$w', want 2..8"

# --- TC-UPG (M4 part): firmware already current -> no firmware transfer ----------------------------
run_device d2 30 --serial ZTP-55AC90 --fw v1.1
expect_line "$T/d2.log" "[UPGRADE] Firmware v1.1 is current, no upgrade required" "firmware current"
n=$(count "$T/d2.log" "Firmware manifest")
[ "$n" -eq 0 ] && ok || bad "firmware current: $n firmware manifests"

# --- TC-PROV-06: chunk boundaries ------------------------------------------------------------------
for c in "B00001 1" "B00002 1" "B00003 1" "B00004 2" "B00005 1024"; do
    set -- $c
    run_device "b$1" 40 --serial "ZTP-$1"
    if [ "$1" = B00001 ]; then   # 1 B: downloads and verifies, but can't hold a firmware header (§12.1)
        expect_line "$T/b$1.log" "Firmware header does not name EDGE-GW-100 v2.1" "TC-PROV-06 ZTP-$1 (header)"
    else
        [ "$DEV_RC" -eq 0 ] && ok || bad "TC-PROV-06 ZTP-$1: device exit $DEV_RC"
    fi
    grep -q "Firmware manifest: v[0-9.]*, [0-9]* bytes, $2 chunks" "$T/b$1.log" && ok || bad "TC-PROV-06 ZTP-$1: want $2 chunks"
    expect_line "$T/b$1.log" "[PROVISION] Firmware checksum verified" "TC-PROV-06 ZTP-$1"
done
stop_server

# --- TC-PROV-11: duplicated chunks are harmless (server TX --dup 0.5) -------------------------------
start_server s2 --dup 0.5 --seed 3
run_device d3 30 --serial ZTP-D00011
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-PROV-11: device exit $DEV_RC"
expect_line "$T/d3.log" "[PROVISION] Firmware: 64 chunks written" TC-PROV-11
# duplicated datagrams are caught by the reliability layer (duplicate seq), so no chunk is ever written
# twice and the application never even sees a duplicate: writes = 64, transport dup counter > 0
grep -q 'Firmware: 64 chunks written, 0 duplicates ignored' "$T/d3.log" && ok || bad "TC-PROV-11: rewrites"
grep -q '\[UDP\] Counters: .* dup=[1-9][0-9]* ' "$T/d3.log" && ok || bad "TC-PROV-11: no duplicates reached the device"
stop_server

# --- TC-REL-07: a chunk lost mid-window is fast-retransmitted; TC-SIM-02 OOO half ----------------------
start_server s3 --test-hooks --drop-seq tx:20
run_device d4 30 --serial ZTP-D00007
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-REL-07: device exit $DEV_RC"
expect_line "$T/s3.log" "[UDP] [sid=" TC-REL-07
expect_line "$T/s3.log" "Retransmitting seq=20 (fast, sack)" TC-REL-07
has "$T/s3.log" "Retransmitting seq=20 (timeout)" && bad "TC-REL-07: seq 20 waited for its RTO" || ok
expect_line "$T/d4.log" "[UDP] Received packet seq=21 (out of order, expected=20)" "TC-SIM-02 (OOO)"
expect_line "$T/d4.log" "[UDP] Buffered packet seq=21" "TC-SIM-02 (OOO)"
stop_server

# --- TC-PROV-07: the artifact differs from its manifest (changed on disk after the server hashed it) ---
start_server s4
printf '\377' | dd of="$A/fw_corrupt" bs=1 seek=17000 conv=notrunc 2>/dev/null
run_device d5 40 --serial ZTP-BAD007
[ "$DEV_RC" -eq 1 ] && ok || bad "TC-PROV-07: device exit $DEV_RC, want 1"
expect_line "$T/d5.log" "[PROVISION] Checksum mismatch, discarding (attempt 1/2)" TC-PROV-07
expect_line "$T/d5.log" "[PROVISION] Checksum mismatch, discarding (attempt 2/2)" TC-PROV-07
expect_line "$T/d5.log" "[STATE] Device is FAILED (reason: HASH_MISMATCH)" TC-PROV-07
n=$(count "$T/d5.log" "Firmware manifest")
[ "$n" -eq 2 ] && ok || bad "TC-PROV-07: $n downloads, want 2 (xfer.hash_attempts)"
stop_server
expect_line "$T/s4.log" "ZTP-BAD007] FAILED (HASH_MISMATCH)" "TC-PROV-07 (server)"
grep -q '"ev":"RECORD","serial":"ZTP-BAD007".*"stage":"FAILED"' "$T/s4.db" && ok || bad "TC-PROV-07: DB stage not FAILED"

# --- resume after kill -9 mid-download (TC-PROV-08 flavour), then TC-PROV-10 (artifact changed) ------
start_server s5 --delay 10
start_device d6 --serial ZTP-2E5000
wait_line "$T/d6.log" "[PROVISION] Firmware 160/1024" 30 || bad "resume: the download never reached 160 chunks"
kill -9 "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null
[ -f "$T/d6.state/xfer_2.map" ] && [ -f "$T/d6.state/xfer_2.part" ] && ok || bad "resume: no persisted partial"
run_device d6 60 --serial ZTP-2E5000
[ "$DEV_RC" -eq 0 ] && ok || bad "resume: device exit $DEV_RC"
line=$(grep -F "[PROVISION] Resuming firmware:" "$T/d6.log" | tail -1)
have=$(echo "$line" | sed -n 's/.*Resuming firmware: \([0-9]*\)\/1024 chunks present, requesting \([0-9]*\).*/\1/p')
req=$(echo "$line" | sed -n 's/.*requesting \([0-9]*\).*/\1/p')
# the map is persisted every xfer.persist_every_chunks (16) chunks and may under-claim by up to that
# many (the kill can land between the progress line and the save, §11.4), but never over-claims
[ -n "$have" ] && [ "$have" -ge 144 ] && [ $((have % 16)) -eq 0 ] && [ $((have + req)) -eq 1024 ] && ok || bad "resume: '$line'"
w2=$(grep -F "Firmware: " "$T/d6.log" | tail -1 | sed -n 's/.*Firmware: \([0-9]*\) chunks written.*/\1/p')
[ -n "$w2" ] && [ "$w2" -eq "$req" ] && ok || bad "resume: wrote $w2 chunks after the restart, want $req"
[ "$(sha "$T/d6.state/fw_slot_B.bin")" = "$(sha "$A/fw_resume")" ] && ok || bad "resume: slot sha differs"
stop_server
# TC-PROV-10: interrupt again with a fresh state, then change the artifact while the device is down
start_server s6 --delay 10
start_device d7 --serial ZTP-2E5000
wait_line "$T/d7.log" "[PROVISION] Firmware 160/1024" 30 || bad "TC-PROV-10: the download never reached 160 chunks"
kill -9 "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null
stop_server
printf '\001' | dd of="$A/fw_resume" bs=1 seek=4000 conv=notrunc 2>/dev/null   # a new artifact (new sha; payload, not header)
start_server s7
run_device d7 60 --serial ZTP-2E5000
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-PROV-10: device exit $DEV_RC"
expect_line "$T/d7.log" "The firmware on the server changed since the partial download: discarding it" TC-PROV-10
expect_line "$T/d7.log" "[PROVISION] Firmware: 1024 chunks written" TC-PROV-10
[ "$(sha "$T/d7.state/fw_slot_B.bin")" = "$(sha "$A/fw_resume")" ] && ok || bad "TC-PROV-10: slot sha differs"
stop_server

# --- TC-NAT-05: rebinding during the transfer --------------------------------------------------------
start_server s8 --delay 5
run_device d8 60 --serial ZTP-B00005 --test-hooks --rebind-after 1
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-NAT-05: device exit $DEV_RC"
grep -q 'ZTP-B00005\] Peer address changed .* (rebinding)' "$T/s8.log" && ok || bad "TC-NAT-05: no rebinding"
n=$(count "$T/s8.log" "ZTP-B00005] Registration successful")
[ "$n" -eq 1 ] && ok || bad "TC-NAT-05: $n registrations, want 1"
expect_line "$T/d8.log" "[PROVISION] Firmware checksum verified" TC-NAT-05
stop_server

# --- TC-PROV-13 (b), (c): small-MTU fallback on the server's path (local, --sim-mtu) ----------------
for mode in emsgsize blackhole; do
    start_server "ms_$mode" --sim-mtu "1000:$mode"
    run_device "m_$mode" 40 --serial ZTP-93AF21
    [ "$DEV_RC" -eq 0 ] && ok || bad "TC-PROV-13 $mode: device exit $DEV_RC"
    if [ "$mode" = emsgsize ]; then
        expect_line "$T/ms_$mode.log" "Path MTU too small for 1200 B (EMSGSIZE): datagram limit now 508" "TC-PROV-13 (b)"
    else
        # §5.8: one silent interval only re-requests; the second one declares the black hole
        expect_line "$T/m_$mode.log" "chunks re-requested before a black hole is declared" "TC-PROV-13 (c) re-request"
        expect_line "$T/m_$mode.log" "[UDP] Suspected PMTU black hole: datagram limit now 508" "TC-PROV-13 (c)"
    fi
    expect_line "$T/m_$mode.log" "Firmware manifest: v1.1, 65536 bytes, 256 chunks" "TC-PROV-13 $mode"
    expect_line "$T/m_$mode.log" "[PROVISION] Firmware checksum verified" "TC-PROV-13 $mode"
    grep -q '^max_dgram=508$' "$T/m_$mode.state/device.state" && ok || bad "TC-PROV-13 $mode: max_dgram not persisted"
    stop_server
done
expect_line "$T/m_emsgsize.log" "Server reduced the datagram limit to 508 (MTU_REDUCED)" "TC-PROV-13 (b) fill frame"

echo "m4 provisioning tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
