#!/bin/sh
set -eu
: "${BIN:?}"

base="$(mktemp -d)"
trap 'rm -rf "$base"' EXIT
opal_home="$base/opal"
external="$base/external"
bin="$base/bin"
mkdir -p "$opal_home/cache/nested" "$external" "$bin"
printf 'state\n' >"$opal_home/config.ini"
printf 'identity\n' >"$opal_home/identity.key"
printf 'cached\n' >"$opal_home/cache/nested/file"
printf 'keep\n' >"$external/sentinel"

cat >"$bin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"${OPAL_TEST_SYSTEMCTL_LOG:?}"
exit 0
EOF
chmod +x "$bin/systemctl"

: >"$base/systemctl.log"
PATH="$bin:$PATH" \
OPAL_TEST_SYSTEMCTL_LOG="$base/systemctl.log" \
OPAL_HOME="$opal_home" \
"$BIN" clean >"$base/clean.out"

test ! -e "$opal_home"
test -f "$external/sentinel"
grep -Fxq -- '--user disable --now opal-host.service' "$base/systemctl.log"
grep -Fxq -- '--user disable --now opal-bridge.service' "$base/systemctl.log"
grep -q '^OPAL state cleaned\.$' "$base/clean.out"

# Cleaning an already-empty installation is deliberately idempotent.
: >"$base/systemctl-second.log"
PATH="$bin:$PATH" \
OPAL_TEST_SYSTEMCTL_LOG="$base/systemctl-second.log" \
OPAL_HOME="$opal_home" \
"$BIN" clean >"$base/clean-second.out"
test ! -e "$opal_home"
grep -q '^OPAL state cleaned\.$' "$base/clean-second.out"

echo 'clean tests passed'
