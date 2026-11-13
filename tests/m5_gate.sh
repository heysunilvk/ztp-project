#!/bin/sh
# M5 gate tests that need their own server instance: impairments, a stopped or restarted server, a stopped
# device, NAT emulation over an OPERATIONAL session, and the PDF example runs (QA §11.1 M5 row).
# usage: tests/m5_gate.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts
# Env: ZTP_PORT (default 31555; +1 = discovery). TC-PDF-04 (the PDF's 10.10.10.5:5555) runs in
# tests/check_appendix_a.sh, on CI.
set -u
SRV=$1
DEV=$2
MKA=$3
PORT=${ZTP_PORT:-31555}
DPORT=$((PORT + 1))
T=${SUITE_DIR:-${RESULTS:+$RESULTS/m5_gate}}; T=${T:-${TMPDIR:-/tmp}/ztp-m5g.$$}   # kept under RESULTS (make test)
mkdir -p "$T/art"
pass=0
fail=0
SRV_PID=
BG_PIDS=
cleanup() {
    for p in $SRV_PID $BG_PIDS; do kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
    if [ "$fail" -ne 0 ]; then echo "  (logs kept in $T)"; elif [ -z "${RESULTS:-}" ]; then rm -rf "$T"; fi
}
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
has() { grep -F -q -- "$2" "$1" 2>/dev/null; }
count() { n=$(grep -F -c -- "$2" "$1" 2>/dev/null); echo "${n:-0}"; }
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }
expect_no_line() { if has "$1" "$2"; then bad "$3: unexpected \"$2\" in $(basename "$1")"; else ok; fi; }
expect_state() { if grep -q "^$2=$3\$" "$T/$1.state/device.state" 2>/dev/null; then ok; else bad "$4: $2 is not $3"; fi; }
sha() {   # SHA-256 of a file; a missing file or tool gives a unique non-hash, so two of them never compare equal
    if command -v shasum > /dev/null 2>&1; then h=$(shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1)
    else h=$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1); fi
    echo "${h:-no-hash:$1}"
}
wait_line() {      # wait_line FILE TEXT SECS: 0 if TEXT appeared in time
    i=0
    while [ $i -lt $(($3 * 20)) ]; do has "$1" "$2" && return 0; sleep 0.05; i=$((i + 1)); done
    return 1
}

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
A="$T/art"
"$MKA" --fw "$A/fw_big" EDGE-GW-100 v2.0 1048576 > /dev/null || { echo "mkartifacts --fw failed"; exit 1; }
FW="$A/fw_EDGE-GW-100_v1.1.bin"
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": {
    "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$FW", "config_version": 3,
                     "config_file": "$A/cfg_v3.json", "tenant": "cruise_ops" },
    "EDGE-GW-050": { "firmware": "v1.0", "firmware_file": "$A/fw_EDGE-GW-050_v1.0.bin", "config_version": 2,
                     "config_file": "$A/cfg_v2.json", "tenant": "cruise_ops", "notes": "Legacy device" } },
  "serials": {
    "ZTP-6B0001": { "firmware": "v2.0", "firmware_file": "$A/fw_big" },
    "ZTP-6B0002": { "firmware": "v2.0", "firmware_file": "$A/fw_big" },
    "ZTP-6B0003": { "firmware": "v2.0", "firmware_file": "$A/fw_big" },
    "ZTP-6C0001": { "nat": { "keepalive_interval_s": 1 } } } }
JSON

start_server() {   # start_server NAME DB [args...]: logs to NAME.log, journal DB.db (shared across restarts)
    name=$1; db=$2; shift 2
    "$SRV" --profile demo --set net.server_port=$PORT --set net.discovery_port=$DPORT --policy "$T/policy.json" \
        --set net.mcast_iface_addr=127.0.0.1 --set paths.log="$T/$name.log" --set paths.db="$T/$db.db" \
        --set paths.dump="$T/$name.dump" "$@" > "$T/$name.out" 2>&1 &
    SRV_PID=$!
    wait_line "$T/$name.log" "Server listening" 4 || bad "server $name did not start: $(cat "$T/$name.out")"
}
stop_server() { [ -n "$SRV_PID" ] && kill -CONT "$SRV_PID" 2>/dev/null; [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null; SRV_PID=; }
dev_cmd() {        # dev_cmd NAME [args...]: the device command line (static discovery unless overridden)
    name=$1; shift
    echo "--profile demo --state-dir $T/$name.state --set paths.log=$T/$name.log --set run.operational_hold_s=1 --set discovery.discovery_port=$DPORT --discover static --server 127.0.0.1:$PORT"
}
start_device() {   # start_device NAME [args...]: background; sets DPID
    name=$1; shift
    # shellcheck disable=SC2046
    "$DEV" $(dev_cmd "$name") "$@" > "$T/$name.out" 2>&1 &
    DPID=$!
    BG_PIDS="$BG_PIDS $DPID"
}
wait_device() {    # wait_device SECS: wait for DPID with a watchdog; DEV_RC
    ( sleep "$1"; kill -CONT "$DPID" 2>/dev/null; kill "$DPID" 2>/dev/null ) &
    wpid=$!
    wait "$DPID" 2>/dev/null
    DEV_RC=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
}
run_device() { name=$1; secs=$2; shift 2; start_device "$name" "$@"; wait_device "$secs"; }
fd_count() {       # fd_count PID: open descriptors of a process
    if [ -d "/proc/$1/fd" ]; then ls "/proc/$1/fd" | wc -l | tr -d ' '
    else lsof -p "$1" 2>/dev/null | awk 'NR > 1 && $4 ~ /^[0-9]+[urwu]?$/' | wc -l | tr -d ' '; fi
}
operational_ok() { # operational_ok NAME TC
    [ "$DEV_RC" -eq 0 ] && ok || bad "$2: device exit $DEV_RC"
    expect_line "$T/$1.log" "[STATE] Device is OPERATIONAL" "$2"
    expect_state "$1" stage OPERATIONAL "$2"
}
regs() { n=$(grep -c "$2\] Registration successful" "$1" 2>/dev/null); echo "${n:-0}"; }

echo "m5 gate tests (ports $PORT/$DPORT)"

# ---------------- one clean server: PDF example runs, legacy device, keepalives, fds ----------------
start_server s1 s1 --dump-on-exit

# TC-PDF-01: first boot with no --serial, static and multicast; TC-PDF-03: the saved config parses as the PDF's
run_device p1 40
expect_line "$T/p1.log" "[BOOT] Soft serial generated" "TC-PDF-01 (static)"
awk '/Soft serial generated/ { g = 1 } g && /Provisioning server found .* \(static\)/ { f = 1 } END { exit !f }' "$T/p1.log" \
    && ok || bad "TC-PDF-01 (static): serial generated, then server found, not in that order"
run_device p2 40 --discover mcast --mcast-iface 127.0.0.1
expect_line "$T/p2.log" "[BOOT] Soft serial generated" "TC-PDF-01 (multicast)"
awk '/Soft serial generated/ { g = 1 } g && /Provisioning server found .* \(OFFER verified\)/ { f = 1 } END { exit !f }' "$T/p2.log" \
    && ok || bad "TC-PDF-01 (multicast): no verified OFFER after the generated serial"
if python3 -I - "$T/p1.state/config.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
assert c["interfaces"]["wan"] == "eth0" and c["interfaces"]["lan"] == "eth1"
assert c["mtu"] == 1500 and c["keepalive_interval"] == 20
PY
then ok; else bad "TC-PDF-03: config.json does not match the PDF example"; fi

# TC-UPG-06: the legacy model upgrades v0.9 -> v1.0 with config v2 and the policy note
run_device l1 40 --serial ZTP-19FE77 --model EDGE-GW-050 --fw v0.9
operational_ok l1 "TC-UPG-06"
expect_line "$T/l1.log" "[UPGRADE] Current: v0.9 -> Target: v1.0" "TC-UPG-06"
expect_line "$T/l1.log" "[VALIDATION] Configuration present (v2, sha ok)" "TC-UPG-06"
expect_state l1 active_slot B "TC-UPG-06"

# TC-NAT-01: OPERATIONAL with demo timers for 20 s: a keepalive about every 2 s, each acknowledged
run_device k1 45 --serial ZTP-6A0001 --fw v1.1 --set run.operational_hold_s=20
operational_ok k1 "TC-NAT-01"
awk '/Device is OPERATIONAL/ { on = 1 } on' "$T/k1.log" > "$T/k1.op"
s=$(count "$T/k1.op" "[NAT] Keepalive sent"); a=$(count "$T/k1.op" "[NAT] Keepalive acknowledged")
[ "$s" -ge 7 ] && [ "$s" -le 12 ] && ok || bad "TC-NAT-01: $s keepalives in 20 s, want about 10"
[ "$a" -ge $((s - 1)) ] && ok || bad "TC-NAT-01: $a of $s keepalives acknowledged"

# TC-NAT-02: no keepalives while the 1 MB firmware is flowing
run_device k2 60 --serial ZTP-6B0001
operational_ok k2 "TC-NAT-02"
awk '/Firmware manifest:/ { on = 1 } on && /Firmware: [0-9]+ chunks written/ { on = 0 } on && /Keepalive sent/ { n++ } END { exit n > 0 }' \
    "$T/k2.log" && ok || bad "TC-NAT-02: keepalive sent during the firmware transfer"

# TC-UPG-02: no descriptors leak across the execv: boot #2 holds as many as a device that never restarted
start_device f0 --serial ZTP-6A0002 --fw v1.1 --set run.operational_hold_s=4
wait_line "$T/f0.log" "[STATE] Device is OPERATIONAL" 30 && n0=$(fd_count "$DPID") || n0=x
wait_device 20
start_device f1 --serial ZTP-6A0003 --set run.operational_hold_s=4
wait_line "$T/f1.log" "[STATE] Device is OPERATIONAL" 40 && n1=$(fd_count "$DPID") || n1=y
wait_device 20
expect_line "$T/f1.log" "[BOOT] Device starting (boot #2)" "TC-UPG-02 (fds)"
# a count of 0 means the count failed (lsof missing or the process gone), not a match
[ "$n0" -gt 0 ] 2>/dev/null && [ "$n0" = "$n1" ] && ok || bad "TC-UPG-02: $n1 descriptors after the execv, $n0 without a restart"

# TC-NAT-09 (OPERATIONAL): the NAT reboots under a live session: rebinding, no re-registration
run_device r1 40 --serial ZTP-6A0004 --fw v1.1 --set run.operational_hold_s=8 --nat-sim reboot_at_s=3
operational_ok r1 "TC-NAT-09 (operational)"
grep -q 'ZTP-6A0004\] Peer address changed .* (rebinding)' "$T/s1.log" && ok || bad "TC-NAT-09 (operational): no rebinding logged"
[ "$(count "$T/r1.log" "[AUTH] REGISTER:")" -eq 1 ] && ok || bad "TC-NAT-09 (operational): registered again"

# TC-NAT-09 (download): the NAT reboots in the middle of the 1 MB firmware download
run_device r2 90 --serial ZTP-6B0002 --delay 30 --nat-sim reboot_at_s=3
operational_ok r2 "TC-NAT-09 (download)"
grep -q 'ZTP-6B0002\] Peer address changed .* (rebinding)' "$T/s1.log" && ok || bad "TC-NAT-09 (download): no rebinding logged"
[ "$(sha "$T/r2.state/fw_slot_B.bin")" = "$(sha "$A/fw_big")" ] && ok || bad "TC-NAT-09 (download): slot B differs"
[ "$(regs "$T/s1.log" ZTP-6B0002)" -eq 1 ] && ok || bad "TC-NAT-09 (download): registered more than once"

# TC-NAT-08: NAT idle timeout (1.5 s) below the keepalive (2 s): rebinding with advice; then a 1 s policy keepalive
run_device n1 60 --serial ZTP-6A0005 --fw v1.1 --set run.operational_hold_s=30 --nat-sim idle_timeout_s=1.5
operational_ok n1 "TC-NAT-08 (run 1)"
grep -q 'ZTP-6A0005\] Rebinding after [0-9]* s idle: NAT idle timeout is probably below keepalive_interval' "$T/s1.log" \
    && ok || bad "TC-NAT-08 (run 1): no advice"
[ "$(count "$T/n1.log" "[AUTH] REGISTER:")" -eq 1 ] && ok || bad "TC-NAT-08 (run 1): registered again"
run_device n2 60 --serial ZTP-6C0001 --fw v1.1 --set run.operational_hold_s=30 --nat-sim idle_timeout_s=1.5
operational_ok n2 "TC-NAT-08 (run 2)"
grep 'ZTP-6C0001\]' "$T/s1.log" | grep -q 'Rebinding after\|Peer address changed' \
    && bad "TC-NAT-08 (run 2): still rebinding with a 1 s keepalive" || ok

stop_server
grep '| ZTP-19FE77 ' "$T/s1.dump" 2>/dev/null | grep -q 'Legacy device' && ok || bad "TC-UPG-06: no \"Legacy device\" note in the dump"
grep '| ZTP-19FE77 ' "$T/s1.dump" 2>/dev/null | grep -q 'v1.0' && ok || bad "TC-UPG-06: dump row without v1.0"

# ---------------- TC-REL-05 / TC-REL-10: duplication and reordering on both sides ----------------
start_server d5 d5 --dup 1.0 --seed 5
run_device x5 90 --serial ZTP-6D0005 --dup 1.0 --seed 6
stop_server
operational_ok x5 "TC-REL-05"
expect_line "$T/x5.log" "Duplicate packet seq=" "TC-REL-05"
expect_line "$T/x5.log" "Firmware: 64 chunks written" "TC-REL-05 (each chunk written once)"
[ "$(regs "$T/d5.log" ZTP-6D0005)" -eq 1 ] && ok || bad "TC-REL-05: registered more than once"
[ "$(grep -c 'ZTP-6D0005\] Device installing' "$T/d5.log")" -eq 1 ] && ok || bad "TC-REL-05: INSTALLING handled twice"
[ "$(grep -c 'ZTP-6D0005\] OPERATIONAL' "$T/d5.log")" -eq 1 ] && ok || bad "TC-REL-05: OPERATIONAL recorded twice"
# every datagram duplicated: the [SIM] totals show duplicated == tx on both sides
for f in "$T/x5.log" "$T/d5.log"; do
    l=$(grep '\[SIM\] tx=' "$f" | tail -1)
    tx=$(echo "$l" | sed -n 's/.*tx=\([0-9]*\).*/\1/p'); du=$(echo "$l" | sed -n 's/.*duplicated=\([0-9]*\).*/\1/p')
    [ -n "$tx" ] && [ "$tx" = "$du" ] && ok || bad "TC-REL-05: $(basename "$f") tx=${tx:-?} duplicated=${du:-?}"
done

start_server d10 d10 --reorder 0.25 --reorder-delay 50 --seed 7
run_device x10 90 --serial ZTP-6D0010 --reorder 0.25 --reorder-delay 50 --seed 8
stop_server
operational_ok x10 "TC-REL-10"
expect_line "$T/x10.log" "(out of order, expected=" "TC-REL-10"
expect_line "$T/x10.log" "Buffered packet seq=" "TC-REL-10"
[ "$(sha "$T/x10.state/fw_slot_B.bin")" = "$(sha "$FW")" ] && ok || bad "TC-REL-10: slot B differs (out-of-order write)"

# ---------------- TC-SESS-05: no server for 20 s, then it starts ----------------
# rto_max 1 s: each HELLO budget (9 transmissions) lasts about 7.5 s, so the 20 s outage spans several backoffs
start_device e1 --serial ZTP-6E0001 --set rel.rto_max_ms=1000
sleep 20
start_server e1s e1s
wait_device 60
stop_server
operational_ok e1 "TC-SESS-05"
[ "$(count "$T/e1.log" "Reconnecting in")" -ge 2 ] && ok || bad "TC-SESS-05: no backoff before the server started"
expect_line "$T/e1.log" "after 9 HELLO transmissions" "TC-SESS-05 (blocked-path hint)"

# ---------------- TC-NAT-03: the server stops while the device is OPERATIONAL ----------------
start_server n3 n3
start_device n3d --serial ZTP-6F0003 --fw v1.1 --set run.operational_hold_s=40
wait_line "$T/n3d.log" "[STATE] Device is OPERATIONAL" 30 || bad "TC-NAT-03: never OPERATIONAL"
kill -STOP "$SRV_PID"
wait_line "$T/n3d.log" "[NAT] Session timeout detected" 15 && ok || bad "TC-NAT-03: no session timeout"
wait_line "$T/n3d.log" "Resuming session" 15 && ok || bad "TC-NAT-03: no resume attempt"
sleep 2
kill -CONT "$SRV_PID"
wait_line "$T/n3d.log" "[UDP] Session resumed (session_id=" 30 && ok || bad "TC-NAT-03: not resumed after SIGCONT"
kill "$DPID" 2>/dev/null; wait_device 10
stop_server
[ "$(count "$T/n3d.log" "[AUTH] REGISTER:")" -eq 1 ] && ok || bad "TC-NAT-03: registered again"

# ---------------- TC-NAT-04: a stopped device: idle, grace, deleted, DB kept, SESSION_EXPIRED ----------------
start_server n4 n4 --set nat.grace_s=12
start_device n4d --serial ZTP-6F0004 --fw v1.1 --set run.operational_hold_s=60
wait_line "$T/n4d.log" "[STATE] Device is OPERATIONAL" 30 || bad "TC-NAT-04: never OPERATIONAL"
kill -STOP "$DPID"
wait_line "$T/n4.log" "ZTP-6F0004] Session idle, entering grace" 15 && ok || bad "TC-NAT-04: no idle detection"
wait_line "$T/n4.log" "ZTP-6F0004] Grace period over" 20 && ok || bad "TC-NAT-04: not deleted after grace_s"
expect_line "$T/n4.log" "DB record kept" "TC-NAT-04"
kill -CONT "$DPID"
wait_line "$T/n4d.log" "Session expired on the server: registering again" 30 && ok || bad "TC-NAT-04: no SESSION_EXPIRED"
wait_line "$T/n4.log" "Resume for an unknown session: SESSION_EXPIRED" 1 && ok || bad "TC-NAT-04: server did not answer SESSION_EXPIRED"
kill "$DPID" 2>/dev/null; wait_device 10
stop_server

# ---------------- TC-VAL-04: the server stops at the start of VALIDATE ----------------
# (a) SIGCONT within val.deadline_s: OPERATIONAL only after the session recovers
start_server v4 v4
start_device v4a --serial ZTP-6A0011 --fw v1.1 --test-hooks --hook-hold-health-probe 2 --set val.deadline_s=25
wait_line "$T/v4a.log" "Health probe held for 2 s" 30 || bad "TC-VAL-04 (a): never reached VALIDATE"
kill -STOP "$SRV_PID"
sleep 8
expect_no_line "$T/v4a.log" "[STATE] Device is OPERATIONAL" "TC-VAL-04 (a): OPERATIONAL while the server is stopped"
kill -CONT "$SRV_PID"
wait_device 40
operational_ok v4a "TC-VAL-04 (a)"
# (b) never resumed: FAILED once val.deadline_s has passed
start_device v4b --serial ZTP-6A0012 --fw v1.1 --test-hooks --hook-hold-health-probe 2 --set val.deadline_s=8
wait_line "$T/v4b.log" "Health probe held for 2 s" 30 || bad "TC-VAL-04 (b): never reached VALIDATE"
kill -STOP "$SRV_PID"
wait_device 40
kill -CONT "$SRV_PID"
stop_server
[ "$DEV_RC" -ne 0 ] && ok || bad "TC-VAL-04 (b): device exit 0"
expect_line "$T/v4b.log" "Session not healthy within val.deadline_s (8 s)" "TC-VAL-04 (b)"
expect_line "$T/v4b.log" "[STATE] Device is FAILED (reason: session unhealthy)" "TC-VAL-04 (b)"

# ---------------- TC-PROV-09: the server restarts in the middle of the firmware download ----------------
start_server p9 p9
start_device p9d --serial ZTP-6B0003 --delay 30
wait_line "$T/p9d.log" "[PROVISION] Firmware 256/1024" 60 || bad "TC-PROV-09: download never reached 256 chunks"
stop_server
start_server p9b p9
wait_device 120
stop_server
operational_ok p9d "TC-PROV-09"
expect_line "$T/p9b.log" "journaled session(s) into IDLE_GRACE" "TC-PROV-09 (restore)"
[ "$(count "$T/p9d.log" "[AUTH] REGISTER:")" -eq 1 ] && ok || bad "TC-PROV-09: registered again"
[ $(($(regs "$T/p9.log" ZTP-6B0003) + $(regs "$T/p9b.log" ZTP-6B0003))) -eq 1 ] && ok || bad "TC-PROV-09: server registered twice"
expect_line "$T/p9b.log" "ZTP-6B0003] Resume accepted" "TC-PROV-09"
grep -q 'Resuming firmware: [1-9][0-9]*/1024 chunks present' "$T/p9d.log" && ok || bad "TC-PROV-09: partial download not reused"
[ "$(sha "$T/p9d.state/fw_slot_B.bin")" = "$(sha "$A/fw_big")" ] && ok || bad "TC-PROV-09: slot B differs"

# ---------------- TC-UPG-11 (b): the server is killed right after accepting a resume ----------------
# The HELLO_ACK(RESUMED) is dropped (server --drop-seq tx:1, slow server RTO so it isn't retransmitted), the
# server is SIGKILLed, a new one starts on the same journal; the device's resume HELLO retransmission (same ctr
# and Nd') must get the rebuilt HELLO_ACK (rule 1): one resume, no re-registration.
start_server u1 u1
run_device u11 40 --serial ZTP-6A0021 --test-hooks --restart-mode exit
[ "$DEV_RC" -eq 75 ] && ok || bad "TC-UPG-11 (b): first run exit $DEV_RC, want 75"
stop_server
start_server u2 u1 --test-hooks --drop-seq tx:1 --set rel.rto_initial_ms=10000 --set rel.rto_min_ms=10000 \
    --set rel.rto_max_ms=20000
start_device u11 --serial ZTP-6A0021 --test-hooks --restart-mode exit --set rel.rto_initial_ms=3000 --set rel.rto_min_ms=3000
wait_line "$T/u2.log" "ZTP-6A0021] Resume accepted" 15 || bad "TC-UPG-11 (b): resume not accepted"
kill -9 "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=
start_server u3 u1
wait_device 60
stop_server
operational_ok u11 "TC-UPG-11 (b)"
expect_line "$T/u3.log" "ZTP-6A0021] Resume (ctr=" "TC-UPG-11 (b)"
expect_line "$T/u3.log" "repeated after a server restart: HELLO_ACK rebuilt" "TC-UPG-11 (b)"
[ "$(count "$T/u11.log" "Resuming session")" -eq 1 ] && ok || bad "TC-UPG-11 (b): more than one resume attempt"
[ "$(count "$T/u11.log" "[AUTH] REGISTER:")" -eq 1 ] && ok || bad "TC-UPG-11 (b): registered again"


echo "m5 gate tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
