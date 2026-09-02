#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}"
base="$(mktemp -d)"; trap 'test -n "${hp:-}" && kill "$hp" 2>/dev/null || true; rm -rf "$base"' EXIT
server="$base/server"; client="$base/client"
OPAL_HOME="$server" "$BIN" host setup >"$base/setup"
password="$(sed -n 's/^Pairing password: //p' "$base/setup")"
test -n "$password"
OPAL_HOME="$server" OPAL_CAPTURE_CMD="printf OPALTEST" OPAL_INPUT_HELPER="$INPUT_BIN" "$BIN" host >"$base/host1.log" 2>&1 & hp=$!
sleep 0.5
OPAL_HOME="$client" OPAL_PLAYER_CMD="cat >/dev/null" DISPLAY= "$BIN" connect 127.0.0.1 "$password" >"$base/client1.log" 2>&1
kill "$hp" 2>/dev/null || true; wait "$hp" 2>/dev/null || true; hp=""
test -s "$server/authorized_clients"
grep -q '^\[127.0.0.1\]' "$client/hosts.ini"
grep -q '^paired=true$' "$client/hosts.ini"
OPAL_HOME="$server" OPAL_CAPTURE_CMD="printf OPALTEST2" OPAL_INPUT_HELPER="$INPUT_BIN" "$BIN" host >"$base/host2.log" 2>&1 & hp=$!
sleep 0.5
OPAL_HOME="$client" OPAL_PLAYER_CMD="cat >/dev/null" DISPLAY= "$BIN" connect 127.0.0.1 >"$base/client2.log" 2>&1
kill "$hp" 2>/dev/null || true; wait "$hp" 2>/dev/null || true; hp=""
grep -q 'Connected' "$base/client2.log"
echo 'integration tests passed'
