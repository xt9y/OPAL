#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}" "${RENDEZVOUS_BIN:?}"
base="$(mktemp -d)"
hp=""; cp=""; rp=""
trap 'test -n "$cp" && kill "$cp" 2>/dev/null || true; test -n "$hp" && kill "$hp" 2>/dev/null || true; test -n "$rp" && kill "$rp" 2>/dev/null || true; rm -rf "$base"' EXIT
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
code="$(sed -n 's/^Connection code: //p' "$base/setup")"
password="$(sed -n 's/^Pairing password: //p' "$base/setup")"
test -n "$code"; test -n "$password"
lower_password="$(printf '%s' "$password" | tr '[:upper:]' '[:lower:]')"

port=$((43000 + ($$ % 15000)))
OPAL_RENDEZVOUS_BIND=127.0.0.1 \
OPAL_RENDEZVOUS_PUBLIC_HOST=127.0.0.1 \
OPAL_RENDEZVOUS_PORT="$port" \
"$RENDEZVOUS_BIN" >"$base/rendezvous.log" 2>&1 & rp=$!
sleep 0.2
kill -0 "$rp"
grep -q 'OPAL rendezvous+relay listening' "$base/rendezvous.log"

OPAL_RENDEZVOUS_HOST=127.0.0.1 \
OPAL_RENDEZVOUS_PORT="$port" \
OPAL_HOME="$server" \
OPAL_CAPTURE_CMD="$base/bin/capture" \
OPAL_TEST_CAPTURE_COUNT="$base/captures.log" \
OPAL_INPUT_HELPER="$INPUT_BIN" \
OPAL_TEST_CLOSE_FIRST_PEER_MS=1500 \
OPAL_TEST_AUTH_LOG="$base/auth.log" \
OPAL_DEBUG=1 \
"$BIN" --internal-host-run >"$base/host.log" 2>&1 & hp=$!
sleep 0.4
kill -0 "$hp"

OPAL_RENDEZVOUS_HOST=127.0.0.1 \
OPAL_RENDEZVOUS_PORT="$port" \
OPAL_AUDIO_TEST_SINK=discard \
OPAL_HOME="$client" \
OPAL_DEBUG=1 \
"$BIN" --internal-connect "$code" "$lower_password" >"$base/client.log" 2>&1 & cp=$!

recovered=0
i=0
while test "$i" -lt 220; do
    captures=0; test -f "$base/captures.log" && captures="$(wc -l <"$base/captures.log")"
    if test "$captures" -ge 2 \
       && test -f "$base/auth.log" && grep -q '^PAIR$' "$base/auth.log" && grep -q '^AUTH$' "$base/auth.log" \
       && grep -q 'Connected' "$base/client.log" \
       && grep -q 'peer session recovered media_generation=2' "$base/client.log"; then recovered=1; break; fi
    kill -0 "$cp" 2>/dev/null || break
    kill -0 "$hp" 2>/dev/null || break
    kill -0 "$rp" 2>/dev/null || break
    sleep 0.1
    i=$((i+1))
done

test "$recovered" -eq 1
kill -0 "$hp"; kill -0 "$cp"; kill -0 "$rp"
grep -q 'Connected' "$base/client.log"
grep -Eq 'OPAL network path=(lan|direct|relay)' "$base/client.log"
! grep -q 'Pairing password:' "$base/client.log"
test -s "$server/authorized_clients"
grep -q '^paired=true$' "$client/hosts.ini"
grep -q '^host_public_key=' "$client/hosts.ini"

first_count="$(wc -l <"$base/captures.log")"
kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""

# The host detects the closed encrypted peer by its bounded liveness timer,
# returns to rendezvous registration, and accepts a fresh paired connection.
sleep 4
kill -0 "$hp"; kill -0 "$rp"

OPAL_RENDEZVOUS_HOST=127.0.0.1 \
OPAL_RENDEZVOUS_PORT="$port" \
OPAL_AUDIO_TEST_SINK=discard \
OPAL_HOME="$client" \
OPAL_DEBUG=1 \
"$BIN" --internal-connect "$code" >"$base/client2.log" 2>&1 & cp=$!
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
kill -0 "$hp"; kill -0 "$cp"; kill -0 "$rp"
grep -q 'Connected' "$base/client2.log"
! grep -q 'Pairing password:' "$base/client2.log"
! grep -qi 'authentication denied' "$base/client2.log"

grep -q '^PAIR$' "$base/auth.log"
auth_count="$(grep -c '^AUTH$' "$base/auth.log")"
test "$auth_count" -ge 2

kill "$cp" 2>/dev/null || true; wait "$cp" 2>/dev/null || true; cp=""
kill "$hp" 2>/dev/null || true; wait "$hp" 2>/dev/null || true; hp=""
kill "$rp" 2>/dev/null || true; wait "$rp" 2>/dev/null || true; rp=""
echo 'integration tests passed'
