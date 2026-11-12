#!/bin/sh
# TC-PDF-05: UDP only, "not a REST or API exercise", C, no UI. The repository holds C sources, shell scripts,
# the Makefile, JSON configuration, the CI workflow and docs; the code has no HTTP or TCP; neither binary links
# listen() or accept(); and at runtime neither process holds a TCP socket.
# usage: tests/check_pdf05.sh path/to/ztp-server path/to/ztp-device   (from the repository root)
set -u
SRV=$1
DEV=$2
pass=0
fail=0
ok()  { pass=$((pass + 1)); }
bad() { echo "  FAIL: $1"; fail=$((fail + 1)); }

if git rev-parse --git-dir > /dev/null 2>&1; then
    others=$(git -c core.quotepath=off ls-files | grep -vE '\.(c|h|sh|md|pdf|json|yml)$|(^|/)Makefile$|(^|/)\.gitignore$')
    [ -z "$others" ] && ok || bad "tracked files that are not C, shell, Makefile, config or docs: $others"
    git -c core.quotepath=off ls-files | grep -qiE '\.(py|js|ts|html|css|go|rs|java)$' && bad "another language's source is tracked" || ok
else
    echo "  (not a git checkout: the file-type check is skipped)"
fi
# no HTTP, REST or TCP in the code (whole words: resume_accept( is not accept()
hits=$(grep -rnE '\bhttps?://|\bHTTP/|SOCK_STREAM|(^|[^_A-Za-z0-9])(listen|accept)\(' --include='*.c' --include='*.h' \
       common server device tools tests 2>/dev/null)
[ -z "$hits" ] && ok || bad "HTTP or TCP in the code: $hits"
for b in "$SRV" "$DEV"; do
    nm -u "$b" 2>/dev/null | grep -qE '^_?(listen|accept|accept4)$' && bad "$(basename "$b") links listen()/accept()" || ok
done
# at runtime: no TCP sockets (the binaries open UDP only)
T=${TMPDIR:-/tmp}/ztp-pdf05.$$
mkdir -p "$T"
"$SRV" --profile demo --set net.server_port=48555 --set net.discovery_port=48556 --set paths.log="$T/s.log" \
    --set paths.db="$T/db" --set paths.dump="$T/dump" > /dev/null 2>&1 &
sp=$!
sleep 1
"$DEV" --profile demo --state-dir "$T/d" --set paths.log="$T/d.log" --serial ZTP-93AF21 --fw v1.1 --discover static \
    --server 127.0.0.1:48555 --set run.exit_when_operational=false > /dev/null 2>&1 &
dp=$!
sleep 3
if command -v lsof > /dev/null 2>&1; then
    tcp=$(lsof -nP -a -iTCP -p "$sp,$dp" 2>/dev/null | tail -n +2)
    [ -z "$tcp" ] && ok || bad "TCP sockets at runtime: $tcp"
    udp=$(lsof -nP -a -iUDP -p "$sp,$dp" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')
    [ "$udp" -ge 2 ] && ok || bad "expected UDP sockets on both processes, found $udp"
else
    echo "  (lsof not available: the runtime socket check is skipped)"
fi
kill "$dp" "$sp" 2>/dev/null
wait "$dp" "$sp" 2>/dev/null
rm -rf "$T"
echo "TC-PDF-05: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
