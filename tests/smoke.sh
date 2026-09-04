#!/bin/sh
set -eu
: "${BIN:?}" "${INPUT_BIN:?}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

OPAL_HOME="$tmp/.opal" "$BIN" version | grep -q '^OPAL 0.2.0$'
OPAL_HOME="$tmp/.opal" "$BIN" help >"$tmp/help.txt"
for command in restart clean select new remove doctor version help; do
    grep -q "opal $command" "$tmp/help.txt"
done
grep -q -- 'opal \[--mode max|1080p|1440p|4k\] \[--fps 15-240\]' "$tmp/help.txt"
for removed in setup init host connect hosts wake bridge tunnel; do
    ! grep -Eq "^[[:space:]]+opal $removed([[:space:]]|$)" "$tmp/help.txt"
done
grep -q 'signed rendezvous, direct end-to-end encrypted UDP' "$tmp/help.txt"
grep -q 'blind encrypted relay fallback' "$tmp/help.txt"
OPAL_HOME="$tmp/.opal" "$BIN" doctor >"$tmp/doctor.txt"
grep -q 'XInput2 raw client input' "$tmp/doctor.txt"
grep -q 'Networking is built into OPAL' "$tmp/doctor.txt"

printf '3\n' | OPAL_HOME="$tmp/fresh" "$BIN" >"$tmp/default.txt"
grep -q '^OPAL SETUP$' "$tmp/default.txt"
printf '3\n' | OPAL_HOME="$tmp/mode" "$BIN" --mode 1080p --fps 30 >"$tmp/mode.txt"
grep -q '^OPAL SETUP$' "$tmp/mode.txt"
printf '3\n' | OPAL_HOME="$tmp/new" "$BIN" new >"$tmp/new.txt"
grep -q '^OPAL SETUP$' "$tmp/new.txt"

set +e
OPAL_HOME="$tmp/.opal" "$BIN" --mode potato >/dev/null 2>"$tmp/mode.err"
rc=$?
set -e
test "$rc" -eq 2
grep -q 'invalid --mode' "$tmp/mode.err"
set +e
OPAL_HOME="$tmp/.opal" "$BIN" --fps 0 >/dev/null 2>"$tmp/fps.err"
rc=$?
set -e
test "$rc" -eq 2
grep -q 'invalid --fps' "$tmp/fps.err"

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
