#!/bin/sh
# M5 integration tests: upgrade, the commit point, restart, resume, validation, rollback (DESIGN §12-§13).
# usage: tests/m5_upgrade.sh path/to/ztp-server path/to/ztp-device path/to/mkartifacts
# Env: ZTP_PORT (default 30555; +1 = discovery).
set -u
SRV=$1
DEV=$2
MKA=$3
PORT=${ZTP_PORT:-30555}
DPORT=$((PORT + 1))
T=${SUITE_DIR:-${RESULTS:+$RESULTS/m5_upgrade}}; T=${T:-${TMPDIR:-/tmp}/ztp-m5.$$}   # kept under RESULTS (make test)
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
expect_line() { if has "$1" "$2"; then ok; else bad "$3: missing \"$2\" in $(basename "$1")"; fi; }
expect_no_line() { if has "$1" "$2"; then bad "$3: unexpected \"$2\" in $(basename "$1")"; else ok; fi; }
expect_state() { if grep -q "^$2=$3\$" "$T/$1.state/device.state" 2>/dev/null; then ok; else bad "$4: $2 is not $3"; fi; }
sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
A="$T/art"
FW="$A/fw_EDGE-GW-100_v1.1.bin"
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": {
    "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$FW", "config_version": 3,
                     "config_file": "$A/cfg_v3.json", "tenant": "cruise_ops" } } }
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
run_device() {     # run_device NAME SECS [args...]: foreground with a watchdog; DEV_RC
    name=$1; secs=$2; shift 2
    "$DEV" --profile demo --state-dir "$T/$name.state" --set paths.log="$T/$name.log" --set run.operational_hold_s=1 \
        --set discovery.discovery_port=$DPORT --discover static --server 127.0.0.1:$PORT "$@" > "$T/$name.out" 2>&1 &
    DPID=$!
    ( sleep "$secs"; kill "$DPID" 2>/dev/null ) &
    wpid=$!
    wait "$DPID" 2>/dev/null   # the crash hooks abort on purpose
    DEV_RC=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
}
# The device reached OPERATIONAL on v1.1 from slot B, and the slot holds exactly the artifact.
expect_upgraded() {   # expect_upgraded NAME TC
    [ "$DEV_RC" -eq 0 ] && ok || bad "$2: device exit $DEV_RC"
    expect_line "$T/$1.log" "[STATE] Device is OPERATIONAL" "$2"
    expect_state "$1" stage OPERATIONAL "$2"
    expect_state "$1" active_slot B "$2"
    expect_state "$1" pending_reboot 0 "$2"
    [ "$(sha "$T/$1.state/fw_slot_B.bin")" = "$(sha "$FW")" ] && ok || bad "$2: slot B sha differs from the artifact"
}

echo "m5 upgrade tests (ports $PORT/$DPORT)"
start_server s1

# TC-UPG-01, TC-UPG-03, TC-VAL-01, TC-VAL-05: download, INSTALLING, commit, execv restart, resume, validate
run_device u1 40 --serial ZTP-500001
expect_upgraded u1 "TC-UPG-01"
for l in "[UPGRADE] Current: v1.0 -> Target: v1.1" "[UPGRADE] Upgrade complete, restarting device" \
         "[BOOT] Device starting (boot #2)" "[BOOT] Restart detected (pending upgrade to v1.1)" \
         "[BOOT] Firmware version: v1.1 (slot B)" "[UDP] Session resumed (session_id=" \
         "[VALIDATION] Configuration present (v3, sha ok)" "[VALIDATION] Firmware v1.1 verified" \
         "[VALIDATION] Session healthy"; do
    expect_line "$T/u1.log" "$l" "TC-UPG-01/TC-VAL-01"
done
[ "$(grep -c '\[AUTH\] REGISTER:' "$T/u1.log")" -eq 1 ] && ok || bad "TC-UPG-03: want exactly one REGISTER"
[ "$(grep -c 'ZTP-500001\] Registration successful' "$T/s1.log")" -eq 1 ] && ok || bad "TC-UPG-03: server registered more than once"
expect_line "$T/s1.log" "Resume accepted" "TC-UPG-03 (server)"
expect_line "$T/s1.log" "Device running v1.1 after the upgrade" "TC-UPG-01 (server)"
grep -q 'ZTP-500001\] OPERATIONAL' "$T/s1.log" && ok || bad "TC-VAL-05: server did not record OPERATIONAL"

# TC-UPG-02: --restart-mode exit (exit 75 for a supervisor; the same command again finishes the job),
# and --restart-mode exec with the binary found through a PATH lookup
run_device u2 40 --serial ZTP-500002 --test-hooks --restart-mode exit
[ "$DEV_RC" -eq 75 ] && ok || bad "TC-UPG-02 (exit): first run exit $DEV_RC, want 75"
expect_state u2 stage FW_INSTALLED "TC-UPG-02 (committed before the exit)"
run_device u2 40 --serial ZTP-500002 --test-hooks --restart-mode exit
expect_upgraded u2 "TC-UPG-02 (exit)"
DEV_SAVE=$DEV
DEV=$(basename "$DEV_SAVE")
PATH="$(cd "$(dirname "$DEV_SAVE")" && pwd):$PATH" run_device u3 40 --serial ZTP-500003
DEV=$DEV_SAVE
expect_upgraded u3 "TC-UPG-02 (exec via PATH)"
expect_line "$T/u3.log" "[BOOT] Device starting (boot #2)" "TC-UPG-02 (exec via PATH)"

# TC-UPG-05, TC-VAL-03 (b): firmware already current: no download, no restart; then the same with a bad version
run_device a1 40 --serial ZTP-500004 --set identity.factory_fw=v1.1
[ "$DEV_RC" -eq 0 ] && ok || bad "TC-UPG-05: device exit $DEV_RC"
expect_line "$T/a1.log" "Firmware v1.1 is current, no upgrade required" "TC-UPG-05"
expect_line "$T/a1.log" "[STATE] Device is OPERATIONAL" "TC-UPG-05"
expect_no_line "$T/a1.log" "Firmware manifest:" "TC-UPG-05"
[ "$(grep -c 'Device starting' "$T/a1.log")" -eq 1 ] && ok || bad "TC-UPG-05: restarted"
run_device a2 40 --serial ZTP-500005 --set identity.factory_fw=v1.1 --test-hooks --hook-bad-fw-version v9.9
[ "$DEV_RC" -ne 0 ] && ok || bad "TC-VAL-03 (b): device exit 0"
expect_line "$T/a2.log" "Firmware check failed: running v9.9, target v1.1" "TC-VAL-03 (b)"
expect_no_line "$T/a2.log" "rolled back" "TC-VAL-03 (b): nothing to roll back to"
expect_line "$T/a2.log" "[STATE] Device is FAILED" "TC-VAL-03 (b)"

# TC-UPG-07 (all five §12.4 points) and TC-UPG-11 (a) (resume-sent): crash, then the same command again
n=10
for p in download downloaded verified committed post-exec resume-sent; do
    s=ZTP-5000$n
    tc="TC-UPG-07 $p"; [ "$p" = resume-sent ] && tc="TC-UPG-11 (a)"
    run_device "c$n" 40 --serial "$s" --test-hooks --crash-at "$p"
    [ "$DEV_RC" -ne 0 ] && ok || bad "$tc: the first run did not crash"
    expect_line "$T/c$n.log" "TEST HOOK FIRED: crash-at $p" "$tc"
    run_device "c$n" 40 --serial "$s" --test-hooks --crash-at "$p"
    expect_upgraded "c$n" "$tc"
    [ "$(grep -c "TEST HOOK FIRED: crash-at $p" "$T/c$n.log")" -eq 1 ] && ok || bad "$tc: the hook fired twice"
    [ "$(grep -c "$s\] Registration successful" "$T/s1.log")" -eq 1 ] && ok || bad "$tc: registered more than once"
    case $p in
    download)    # the partial download resumes: fewer than all 64 chunks written in the second process
        w=$(grep 'Firmware: [0-9]* chunks written' "$T/c$n.log" | tail -1 | sed 's/.*Firmware: \([0-9]*\) chunks.*/\1/')
        [ -n "$w" ] && [ "$w" -lt 64 ] && ok || bad "$tc: second run wrote ${w:-?} chunks, want < 64" ;;
    verified)    # staged and verified: no second download
        [ "$(grep -c 'Firmware manifest:' "$T/c$n.log")" -eq 1 ] && ok || bad "$tc: firmware downloaded again" ;;
    committed|post-exec)
        expect_line "$T/c$n.log" "[BOOT] Firmware version: v1.1 (slot B)" "$tc" ;;
    esac
    n=$((n + 1))
done

# TC-UPG-10: crash mid-download and relaunch at once: the server session is still live, so the device resumes it
run_device f1 40 --serial ZTP-500016 --test-hooks --crash-after-chunks 40
[ "$DEV_RC" -ne 0 ] && ok || bad "TC-UPG-10: the first run did not crash"
run_device f1 40 --serial ZTP-500016 --test-hooks --crash-after-chunks 40
expect_upgraded f1 "TC-UPG-10"
[ "$(grep -c '\[AUTH\] REGISTER:' "$T/f1.log")" -eq 1 ] && ok || bad "TC-UPG-10: registered again"
[ "$(grep -c 'ZTP-500016\] Resume accepted' "$T/s1.log")" -ge 2 ] && ok || bad "TC-UPG-10: want a resume after the crash and after the upgrade"

# TC-UPG-08: the restart takes longer than the server's grace period: SESSION_EXPIRED, then a fresh REGISTER
stop_server
start_server s2 --set nat.grace_s=12 --dump-on-exit --dump-verbose   # 12 s: the minimum the demo profile allows
run_device g1 40 --serial ZTP-500020 --test-hooks --restart-mode exit --restart-delay 17
[ "$DEV_RC" -eq 75 ] && ok || bad "TC-UPG-08: first run exit $DEV_RC, want 75"
run_device g1 40 --serial ZTP-500020 --test-hooks --restart-mode exit --restart-delay 17
expect_upgraded g1 "TC-UPG-08"
expect_line "$T/g1.log" "Session expired on the server: registering again" "TC-UPG-08"
expect_line "$T/s2.log" "Grace period over" "TC-UPG-08 (server)"
expect_line "$T/g1.log" "Server reports this device was already registered" "TC-UPG-08 (same DB row)"
stop_server
[ "$(grep -c '| ZTP-500020 ' "$T/db_dump_verbose.txt" 2>/dev/null)" -eq 1 ] && ok || bad "TC-UPG-08: want exactly one DB row"
expect_line "$T/db_dump_verbose.txt" "Re-registered after session expiry" "TC-UPG-08 (verbose dump note)"
expect_no_line "$T/s2.dump" "Re-registered" "TC-UPG-08 (default dump Notes unchanged)"
rm -f "$T/db_dump_verbose.txt"
start_server s3 --dump-on-exit --dump-verbose

# TC-UPG-09: the new firmware fails validation: roll back to slot A, report FAILED VALIDATION_FAILED
run_device r1 40 --serial ZTP-500030 --test-hooks --fail-validate
[ "$DEV_RC" -ne 0 ] && ok || bad "TC-UPG-09: device exit 0 after a failed validation"
expect_line "$T/r1.log" "Validation failed on v1.1, rolled back to v1.0" "TC-UPG-09"
expect_line "$T/r1.log" "[STATE] Device is FAILED" "TC-UPG-09"
expect_state r1 active_slot A "TC-UPG-09"
expect_state r1 pending_reboot 0 "TC-UPG-09"
expect_line "$T/s3.log" "ZTP-500030] Device rolled back to v1.0" "TC-UPG-09 (server)"

# TC-VAL-03 (a): after the upgrade the active slot's header names another version: roll back too
run_device r2 40 --serial ZTP-500031 --test-hooks --hook-bad-fw-version v9.9
expect_line "$T/r2.log" "Firmware check failed: running v9.9, target v1.1" "TC-VAL-03 (a)"
expect_line "$T/r2.log" "rolled back to v1.0" "TC-VAL-03 (a)"
expect_state r2 active_slot A "TC-VAL-03 (a)"

# TC-VAL-02: config.json deleted before validation: fetched once more, then OPERATIONAL;
# with --no-refetch: FAILED
run_device v1 40 --serial ZTP-500040 --test-hooks --hook-delete-config
expect_upgraded v1 "TC-VAL-02"
expect_line "$T/v1.log" "Configuration check failed (config.json missing): fetching it again" "TC-VAL-02"
run_device v2 40 --serial ZTP-500041 --test-hooks --hook-delete-config --no-refetch
[ "$DEV_RC" -ne 0 ] && ok || bad "TC-VAL-02 (--no-refetch): device exit 0"
expect_line "$T/v2.log" "[STATE] Device is FAILED (reason: config missing or invalid)" "TC-VAL-02 (--no-refetch)"

# TC-NAT-10 (false) / TC-CFG-10 (f): strict IP-change mode: RESUME_REQUIRED, then a K_dev resume, no REGISTER
if [ "$(uname)" = Linux ] || ifconfig lo0 2>/dev/null | grep -q 'inet 127.0.0.2 '; then
    stop_server
    start_server s4 --set nat.allow_ip_change=false
    # firmware already current (no restart, so the per-process IP-change timer can't be outrun), held OPERATIONAL
    # for 10 s; the IP changes at 3 s and the next keepalive comes from 127.0.0.2
    run_device n1 60 --serial ZTP-500060 --set identity.factory_fw=v1.1 --set run.operational_hold_s=10 \
        --nat-sim change_ip_at_s=3
    [ "$DEV_RC" -eq 0 ] && ok || bad "TC-NAT-10 (strict): device exit $DEV_RC"
    expect_line "$T/n1.log" "[STATE] Device is OPERATIONAL" "TC-NAT-10 (strict)"
    expect_line "$T/n1.log" "[UDP] Session resumed (session_id=" "TC-NAT-10 (strict)"
    if grep -q 'ZTP-500060\] Peer IP changed 127.0.0.1:[0-9]* -> 127.0.0.2:[0-9]*: RESUME_REQUIRED sent' "$T/s4.log"; then ok
    else bad "TC-NAT-10 (strict): no RESUME_REQUIRED in s4.log"; fi
    expect_line "$T/n1.log" "[UDP] Resume required by server (IP change)" "TC-NAT-10 (strict)"
    [ "$(grep -c '\[AUTH\] REGISTER:' "$T/n1.log")" -eq 1 ] && ok || bad "TC-NAT-10 (strict): registered more than once"
    stop_server
    start_server s5
else
    echo "  TC-NAT-10 (strict): skipped (127.0.0.2 is not local: add an lo0 alias on macOS)"
fi

# Hooks are refused without --test-hooks
run_device h1 10 --serial ZTP-500050 --crash-at committed
[ "$DEV_RC" -eq 64 ] && ok || bad "--crash-at without --test-hooks: exit $DEV_RC, want 64"

stop_server
grep '| ZTP-500030 ' "$T/db_dump_verbose.txt" 2>/dev/null | grep -q 'Success' && ok || bad "TC-UPG-09: DB registration is not Success"
grep '| ZTP-500030 ' "$T/db_dump_verbose.txt" 2>/dev/null | grep -q 'Rolled back after failed validation' && ok \
    || bad "TC-UPG-09: no rollback note in the verbose dump"
echo "m5 upgrade tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
