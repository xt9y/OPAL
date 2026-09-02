#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

OPAL_HOME="$tmp/.opal" "$BIN" version | grep -q '^OPAL 0.1.0$'
OPAL_HOME="$tmp/.opal" "$BIN" help >"$tmp/help.txt"
for command in restart clean select new remove doctor version help; do
    grep -q "opal $command" "$tmp/help.txt"
done
for removed in setup init host connect hosts wake bridge tunnel; do
    ! grep -Eq "^[[:space:]]+opal $removed([[:space:]]|$)" "$tmp/help.txt"
done
OPAL_HOME="$tmp/.opal" "$BIN" doctor >"$tmp/doctor.txt"
grep -q 'XInput2 raw client input' "$tmp/doctor.txt"

printf '3\n' | OPAL_HOME="$tmp/fresh" "$BIN" >"$tmp/default.txt"
grep -q '^OPAL SETUP$' "$tmp/default.txt"
printf '3\n' | OPAL_HOME="$tmp/new" "$BIN" new >"$tmp/new.txt"
grep -q '^OPAL SETUP$' "$tmp/new.txt"

set +e
OPAL_HOME="$tmp/.opal" "$BIN" host >/dev/null 2>"$tmp/removed.err"
rc=$?
set -e
test "$rc" -eq 2
grep -q "Unknown command. Run 'opal help'." "$tmp/removed.err"

mkdir -p "$tmp/bin" "$tmp/.opal"
cat >"$tmp/bin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$OPAL_TEST_SYSTEMCTL_LOG"
EOF
chmod +x "$tmp/bin/systemctl"
: >"$tmp/systemctl.log"
touch "$tmp/.opal/restart-sentinel"
PATH="$tmp/bin:$PATH" OPAL_TEST_SYSTEMCTL_LOG="$tmp/systemctl.log" OPAL_HOME="$tmp/.opal" "$BIN" restart >"$tmp/restart.txt"
grep -Fxq -- '--user daemon-reload' "$tmp/systemctl.log"
grep -Fxq -- '--user try-restart opal-host.service' "$tmp/systemctl.log"
grep -Fxq -- '--user try-restart opal-bridge.service' "$tmp/systemctl.log"
test -f "$tmp/.opal/restart-sentinel"

"$INPUT_BIN" </dev/null >/dev/null 2>&1 || true
echo 'smoke tests passed'
