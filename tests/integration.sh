#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}"
base="$(mktemp -d)"
hp=""; cp=""
trap 'test -n "$cp" && kill "$cp" 2>/dev/null || true; test -n "$hp" && kill "$hp" 2>/dev/null || true; rm -rf "$base"' EXIT
server="$base/server"; client="$base/client"
mkdir -p "$base/bin"

cat >"$base/bin/capture" <<'EOF'
#!/bin/sh
printf 'start\n' >>"${OPAL_TEST_CAPTURE_COUNT:?}"
exec ffmpeg -hide_banner -loglevel error -re \
  -f lavfi -i testsrc=size=320x180:rate=60 \
  -f lavfi -i anullsrc=r=48000:cl=stereo \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency \
  -bf 0 -g 15 -keyint_min 15 -sc_threshold 0 \
  -c:a aac -b:a 96k -f flv pipe:1
EOF
chmod +x "$base/bin/capture"
: >"$base/captures.log"

OPAL_HOME="$server" "$BIN" --internal-host-setup >"$base/setup"
password="$(sed -n 's/^Pairing password: //p' "$base/setup")"
test -n "$password"
lower_password="$(printf '%s' "$password" | tr '[:upper:]' '[:lower:]')"

OPAL_HOME="$server" \
OPAL_DISABLE_STUN=1 \
OPAL_CAPTURE_CMD="$base/bin/capture" \
OPAL_TEST_CAPTURE_COUNT="$base/captures.log" \
OPAL_INPUT_HELPER="$INPUT_BIN" \
OPAL_TEST_CONTROL_CLOSE_AFTER_PINGS=1 \
OPAL_TEST_AUTH_LOG="$base/auth.log" \
"$BIN" --internal-host-run >"$base/host.log" 2>&1 & hp=$!
sleep 0.5
kill -0 "$hp"
! grep -q '47991' "$base/host.log"

OPAL_DISABLE_STUN=1 OPAL_AUDIO_TEST_SINK=discard OPAL_HOME="$client" \
"$BIN" --internal-connect 127.0.0.1 "$lower_password" >"$base/client.log" 2>&1 & cp=$!

recovered=0
i=0
while test "$i" -lt 180; do
    captures=0; test -f "$base/captures.log" && captures="$(wc -l <"$base/captures.log")"
    if test "$captures" -ge 2 \
       && test -f "$base/auth.log" && grep -q '^PAIR$' "$base/auth.log" && grep -q '^AUTH$' "$base/auth.log" \
       && grep -q 'Connected' "$base/client.log" \
       && grep -q 'Control interrupted; recovering direct session...' "$base/client.log" \
       && grep -q 'Control restored. Direct video rekeyed.' "$base/client.log"; then recovered=1; break; fi
    kill -0 "$cp" 2>/dev/null || break
    sleep 0.1
    i=$((i+1))
done

test "$recovered" -eq 1
kill -0 "$hp"
kill -0 "$cp"
grep -q 'Connected' "$base/client.log"
! grep -qi 'ffplay' "$base/client.log"
! grep -q 'Pairing password:' "$base/client.log"
test -s "$server/authorized_clients"
grep -q '^\[127.0.0.1\]' "$client/hosts.ini"
grep -q '^paired=true$' "$client/hosts.ini"
grep -q '^mouse_sensitivity=1.0$' "$client/hosts.ini"

first_count="$(wc -l <"$base/captures.log")"
kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""
sleep 0.2
kill -0 "$hp"

OPAL_DISABLE_STUN=1 OPAL_AUDIO_TEST_SINK=discard OPAL_HOME="$client" \
"$BIN" --internal-connect 127.0.0.1 >"$base/client2.log" 2>&1 & cp=$!
second_connected=0
i=0
while test "$i" -lt 120; do
    count="$(wc -l <"$base/captures.log")"
    if test "$count" -gt "$first_count" && grep -q 'Connected' "$base/client2.log"; then second_connected=1; break; fi
    kill -0 "$cp" 2>/dev/null || break
    sleep 0.1
    i=$((i+1))
done

test "$second_connected" -eq 1
kill -0 "$hp"
kill -0 "$cp"
grep -q 'Connected' "$base/client2.log"
! grep -q 'Pairing password:' "$base/client2.log"
! grep -q 'authentication denied' "$base/client2.log"
! grep -qi 'ffplay' "$base/client2.log"

kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""
kill "$hp" 2>/dev/null || true; wait "$hp" 2>/dev/null || true; hp=""
echo 'integration tests passed'
