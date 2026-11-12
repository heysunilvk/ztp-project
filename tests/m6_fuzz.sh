#!/bin/sh
# TC-SRV-07: malformed and mutated datagrams into the server's control and discovery handlers, in process
# (the --self-fuzz test hook; see server/fuzz.c). Run under the ASan build for the gate: no crash, hang or
# sanitizer report, and the error counters move.
# usage: tests/m6_fuzz.sh path/to/ztp-server path/to/mkartifacts
# Env: FUZZ_DGRAMS (per seed, default 100000), FUZZ_SEEDS (default "1 2 3"), ZTP_PORT (default 39555).
set -u
SRV=$1
MKA=$2
N=${FUZZ_DGRAMS:-100000}
SEEDS=${FUZZ_SEEDS:-1 2 3}
PORT=${ZTP_PORT:-39555}   # the sockets are opened (normal startup) but no traffic is read from them
T=${TMPDIR:-/tmp}/ztp-fuzz.$$
mkdir -p "$T/art"
pass=0
fail=0
cleanup() { if [ "$fail" -eq 0 ]; then rm -rf "$T"; else echo "  (logs kept in $T)"; fi; }
trap cleanup EXIT
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }
field() { echo "$1" | sed -n "s/.*$2 +\([0-9]*\).*/\1/p"; }

"$MKA" --out "$T/art" > /dev/null || { echo "mkartifacts failed"; exit 1; }
cat > "$T/policy.json" <<JSON
{ "schema_version": 1, "default": { "action": "reject" }, "tenants": { "cruise_ops": {} },
  "models": { "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "$T/art/fw_EDGE-GW-100_v1.1.bin", "config_version": 3,
                               "config_file": "$T/art/cfg_v3.json", "tenant": "cruise_ops" } } }
JSON

echo "m6 fuzz tests ($N datagrams x seeds $SEEDS)"
for seed in $SEEDS; do
    "$SRV" --profile demo --test-hooks --self-fuzz "$N" --fuzz-seed "$seed" --set net.server_port=$((PORT + 2 * seed)) \
        --set net.discovery_port=$((PORT + 2 * seed + 1)) --policy "$T/policy.json" --set paths.log="$T/s$seed.log" \
        --set paths.db="$T/s$seed.db" --set paths.dump="$T/s$seed.dump" > "$T/s$seed.out" 2>&1 &
    pid=$!
    ( sleep 600; kill -9 "$pid" 2>/dev/null ) > /dev/null 2>&1 &   # detached: an orphan sleep must not hold stdout
    wpid=$!
    wait "$pid"
    rc=$?
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    [ "$rc" -eq 0 ] && ok || bad "seed $seed: exit $rc (crash, sanitizer report, hang or broken invariant)"
    if grep -q 'ERROR: AddressSanitizer\|runtime error:\|ERROR: LeakSanitizer' "$T/s$seed.out"; then
        bad "seed $seed: sanitizer report"; grep -m3 'ERROR\|runtime error' "$T/s$seed.out"
    else ok; fi
    l=$(grep '\[FUZZ\] [0-9]* inputs (' "$T/s$seed.log")
    [ -n "$l" ] && ok || { bad "seed $seed: no summary line"; continue; }
    echo "  seed $seed:$(echo "$l" | sed 's/.*\[FUZZ\]//' | cut -c1-200)"
    for c in malformed unknown mac_fail; do
        v=$(field "$l" "$c"); [ "${v:-0}" -gt 0 ] && ok || bad "seed $seed: $c did not increase"
    done
    echo "$l" | grep -q ', 0 left after expiry' && ok || bad "seed $seed: sessions left after every timer expired"
done
echo "m6 fuzz tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
