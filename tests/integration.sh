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
if test -n "${OPAL_TEST_PLAYER_DRIVER:-}"; then
    printf '%s\n' "${SDL_VIDEODRIVER:-}" >"$OPAL_TEST_PLAYER_DRIVER"
fi
echo 'FFPLAY_DEBUG_NOISE' >&2
head -c 8 >/dev/null || true
exit 0
EOF
chmod +x "$base/bin/ffplay"

OPAL_HOME="$server" "$BIN" --internal-host-setup >"$base/setup"
password="$(sed -n 's/^Pairing password: //p' "$base/setup")"
test -n "$password"

OPAL_HOME="$server" \
OPAL_CAPTURE_CMD="while :; do printf OPALTEST; sleep 0.05; done" \
OPAL_INPUT_HELPER="$INPUT_BIN" \
OPAL_TEST_CONTROL_CLOSE_AFTER_PINGS=1 \
OPAL_TEST_AUTH_LOG="$base/auth.log" \
OPAL_TEST_VIDEO_TOKEN_LOG="$base/video-tokens.log" \
"$BIN" --internal-host-run >"$base/host.log" 2>&1 & hp=$!
sleep 0.5
! grep -q '^Password ' "$base/host.log"

OPAL_TEST_PLAYER_COUNT="$base/player.count" OPAL_TEST_PLAYER_DRIVER="$base/player.driver" PATH="$base/bin:$PATH" OPAL_HOME="$client" DISPLAY=:99 WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=x11 "$BIN" --internal-connect 127.0.0.1 "$password" >"$base/client.log" 2>&1 & cp=$!
recovered=0
i=0
while test "$i" -lt 140; do
    distinct_tokens=0
    if test -f "$base/video-tokens.log"; then distinct_tokens="$(sort -u "$base/video-tokens.log" | wc -l)"; fi
    if test -f "$base/player.count" && test "$(cat "$base/player.count")" -ge 2 \
       && test -f "$base/auth.log" && grep -q '^PAIR$' "$base/auth.log" && grep -q '^AUTH$' "$base/auth.log" \
       && test "$distinct_tokens" -ge 2 \
       && grep -q 'Connected' "$base/client.log" \
       && grep -q 'Control interrupted; recovering...' "$base/client.log" \
       && grep -q 'Control restored.' "$base/client.log" \
       && grep -q 'Video restored.' "$base/client.log"; then recovered=1; break; fi
    kill -0 "$cp" 2>/dev/null || break
    sleep 0.1
    i=$((i+1))
done

test "$recovered" -eq 1
kill -0 "$hp"
kill -0 "$cp"
grep -q 'Connected' "$base/client.log"
grep -q 'Video interrupted; recovering...' "$base/client.log"
grep -q 'Video restored.' "$base/client.log"
grep -q 'Control restored.' "$base/client.log"
! grep -q 'Pairing password:' "$base/client.log"
grep -qx 'wayland' "$base/player.driver"
! grep -q 'FFPLAY_DEBUG_NOISE' "$base/client.log"
! grep -q '^capture:' "$base/host.log"

test -s "$server/authorized_clients"
grep -q '^\[127.0.0.1\]' "$client/hosts.ini"
grep -q '^paired=true$' "$client/hosts.ini"
grep -q '^mouse_sensitivity=1.0$' "$client/hosts.ini"

first_count="$(cat "$base/player.count")"
kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""
sleep 0.2
kill -0 "$hp"

OPAL_TEST_PLAYER_COUNT="$base/player.count" OPAL_TEST_PLAYER_DRIVER="$base/player.driver" PATH="$base/bin:$PATH" OPAL_HOME="$client" DISPLAY=:99 WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=x11 "$BIN" --internal-connect 127.0.0.1 >"$base/client2.log" 2>&1 & cp=$!
second_connected=0
i=0
while test "$i" -lt 100; do
    if test -f "$base/player.count" && test "$(cat "$base/player.count")" -gt "$first_count" && grep -q 'Connected' "$base/client2.log"; then second_connected=1; break; fi
    kill -0 "$cp" 2>/dev/null || break
    sleep 0.1
    i=$((i+1))
done

test "$second_connected" -eq 1
kill -0 "$hp"
kill -0 "$cp"
grep -q 'Connected' "$base/client2.log"
grep -qx 'wayland' "$base/player.driver"
! grep -q 'Pairing password:' "$base/client2.log"
! grep -q 'authentication denied' "$base/client2.log"

kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""
kill "$hp" 2>/dev/null || true; wait "$hp" 2>/dev/null || true; hp=""
echo 'integration tests passed'
