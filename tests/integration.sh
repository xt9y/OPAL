#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}"
base="$(mktemp -d)"
hp=""; cp=""
trap 'test -n "$cp" && kill "$cp" 2>/dev/null || true; test -n "$hp" && kill "$hp" 2>/dev/null || true; rm -rf "$base"' EXIT
server="$base/server"; client="$base/client"
mkdir -p "$base/bin"
cat >"$base/bin/ffplay" <<'EOF'
#!/bin/sh
count_file="${OPAL_TEST_PLAYER_COUNT:?}"
n=0
if test -f "$count_file"; then n="$(cat "$count_file")"; fi
n=$((n+1))
printf '%s\n' "$n" >"$count_file"
echo 'FFPLAY_DEBUG_NOISE' >&2
head -c 8 >/dev/null || true
exit 0
EOF
chmod +x "$base/bin/ffplay"

OPAL_HOME="$server" "$BIN" host setup >"$base/setup"
password="$(sed -n 's/^Pairing password: //p' "$base/setup")"
test -n "$password"

OPAL_HOME="$server" OPAL_CAPTURE_CMD="while :; do printf OPALTEST; sleep 0.05; done" OPAL_INPUT_HELPER="$INPUT_BIN" "$BIN" host >"$base/host.log" 2>&1 & hp=$!
sleep 0.5

OPAL_TEST_PLAYER_COUNT="$base/player.count" PATH="$base/bin:$PATH" OPAL_HOME="$client" DISPLAY= "$BIN" connect 127.0.0.1 "$password" >"$base/client.log" 2>&1 & cp=$!
recovered=0
i=0
while test "$i" -lt 100; do
    if test -f "$base/player.count" && test "$(cat "$base/player.count")" -ge 2; then recovered=1; break; fi
    kill -0 "$cp" 2>/dev/null || break
    sleep 0.1
    i=$((i+1))
done

test "$recovered" -eq 1
kill -0 "$hp"
kill -0 "$cp"
grep -q 'Connected' "$base/client.log"
! grep -q 'FFPLAY_DEBUG_NOISE' "$base/client.log"
! grep -q '^capture:' "$base/host.log"

test -s "$server/authorized_clients"
grep -q '^\[127.0.0.1\]' "$client/hosts.ini"
grep -q '^paired=true$' "$client/hosts.ini"

kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""
kill "$hp" 2>/dev/null || true; wait "$hp" 2>/dev/null || true; hp=""
echo 'integration tests passed'
