#!/bin/sh
# CLI-level configuration checks (TC-CFG-01, -03, -04, -05, -07) against the real binaries.
# usage: tests/cli_config.sh path/to/ztp-server path/to/ztp-device
set -u
SRV=$1
DEV=$2
fail=0
pass=0
expect() {   # expect <exit-code> <description> <command...>
    want=$1; desc=$2; shift 2
    "$@" >/dev/null 2>&1
    got=$?
    if [ "$got" -eq "$want" ]; then pass=$((pass + 1)); else echo "  FAIL: $desc (exit $got, want $want)"; fail=$((fail + 1)); fi
}
src_of() {   # src_of <binary> <section> <key> [args...]: prints "value|source"
    bin=$1; sec=$2; key=$3; shift 3
    "$bin" "$@" --print-config 2>/dev/null | sed -n "/^  \"$sec\": {/,/^  }/p" |
        sed -n "s/^ *\"$key\": {\"value\": \(.*\), \"source\": \"\([^\"]*\)\"}.*/\1|\2/p"
}
check_eq() {
    if [ "$2" = "$3" ]; then pass=$((pass + 1)); else echo "  FAIL: $1: got '$2', want '$3'"; fail=$((fail + 1)); fi
}

ROOT=$(pwd)
T=${TMPDIR:-/tmp}/ztp-cli.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT

# TC-CFG-01: built-in defaults when no config file is present
check_eq "defaults keepalive (cwd)" "$(cd "$T" && src_of "$ROOT/$SRV" nat keepalive_interval_s)" "20|default"

# TC-CFG-03: precedence defaults < file < profile < CLI
mkdir -p "$T/conf/profiles/server"
cp conf/profiles/server/demo.json "$T/conf/profiles/server/"
printf '{"schema_version": 1, "nat": {"keepalive_interval_s": 5}}\n' > "$T/conf/server.json"
check_eq "cli wins" "$(src_of "$SRV" nat keepalive_interval_s --config "$T/conf/server.json" --profile demo --set nat.keepalive_interval_s=3)" "3|cli"
check_eq "profile over file" "$(src_of "$SRV" nat keepalive_interval_s --config "$T/conf/server.json" --profile demo)" "2|profile:demo"
check_eq "file over default" "$(src_of "$SRV" nat keepalive_interval_s --config "$T/conf/server.json")" "5|file:$T/conf/server.json"
check_eq "--demo-timers alias" "$(src_of "$SRV" nat keepalive_interval_s --demo-timers)" "2|profile:demo"
check_eq "shorthand flag" "$(src_of "$SRV" sim loss --loss 0.15)" "0.15|cli"
check_eq "device shorthand (not first key)" "$(src_of "$DEV" identity factory_fw --fw v0.9)" "\"v0.9\"|cli"
check_eq "device shorthand" "$(src_of "$DEV" identity serial --serial ZTP-93AF21)" "\"ZTP-93AF21\"|cli"

# TC-CFG-04: range/type violations exit 78
for bad in 'nat.keepalive_interval_s=0' 'net.max_datagram=1201' 'xfer.chunk_size=2048' 'rel.tx_window=0' 'nat.grace_s=10' 'sim.loss=2'; do
    expect 78 "range $bad" "$SRV" --set "$bad"
done
printf '{"nat": {"keepalive_intervl_s": 20}}\n' > "$T/unknown_key.json"
# TC-CFG-05: unknown key strict (78) vs lenient (0)
expect 78 "unknown key strict" "$SRV" --config "$T/unknown_key.json" --print-config
expect 0 "unknown key lenient" "$SRV" --config "$T/unknown_key.json" --config-lenient --print-config
msg=$("$SRV" --config "$T/unknown_key.json" 2>&1 >/dev/null)
case "$msg" in *"unknown key nat.keepalive_intervl_s"*) pass=$((pass + 1)) ;; *) echo "  FAIL: unknown-key message: $msg"; fail=$((fail + 1)) ;; esac
msg=$("$SRV" --set nat.keepalive_interval_s=0 2>&1 >/dev/null)
check_eq "range message" "$msg" "config: nat.keepalive_interval_s=0 (cli): must be 1..300"

# usage errors exit 64
expect 64 "unknown flag" "$SRV" --no-such-flag
expect 64 "missing argument" "$DEV" --loss
expect 64 "device rejects server flag" "$DEV" --port 5555

# TC-CFG-07: every shipped profile loads cleanly on both sides
for p in default demo strict-nat cgnat lan metered; do
    expect 0 "profile $p (server)" "$SRV" --profile "$p" --print-config
    expect 0 "profile $p (device)" "$DEV" --profile "$p" --print-config
done
expect 78 "unknown profile" "$SRV" --profile nosuch --print-config

echo "cli config tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
