#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

OPAL_HOME="$tmp/.opal" "$BIN" version | grep -q '^OPAL 0.1.0$'
OPAL_HOME="$tmp/.opal" "$BIN" help >"$tmp/help.txt"
grep -q 'Advanced commands:' "$tmp/help.txt"
grep -q 'opal host' "$tmp/help.txt"

printf '3\n' | OPAL_HOME="$tmp/fresh" "$BIN" >"$tmp/default.txt"
grep -q '^OPAL SETUP$' "$tmp/default.txt"
! grep -q 'Usage:' "$tmp/default.txt"
! grep -q 'opal host' "$tmp/default.txt"

OPAL_HOME="$tmp/.opal" "$BIN" init >/dev/null
test -f "$tmp/.opal/config.ini"
test -f "$tmp/.opal/identity.key"
OPAL_HOME="$tmp/.opal" "$BIN" host setup >"$tmp/setup.txt"
grep -q 'Pairing password:' "$tmp/setup.txt"
OPAL_HOME="$tmp/.opal" "$BIN" hosts add desktop 127.0.0.1 00:11:22:33:44:55 >/dev/null
OPAL_HOME="$tmp/.opal" "$BIN" hosts list | grep -q 'desktop'
"$INPUT_BIN" </dev/null >/dev/null 2>&1 || true
echo 'smoke tests passed'
