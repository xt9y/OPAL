#!/bin/sh
set -eu

script="./scripts/configure-firewall.sh"
base="$(mktemp -d)"
trap 'rm -rf "$base"' EXIT
mkdir -p "$base/bin"

cat >"$base/bin/firewall-cmd" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "--state" ]; then
  echo running
  exit 0
fi
printf '%s\n' "$*" >>"${OPAL_TEST_FIREWALL_LOG:?}"
EOF
chmod +x "$base/bin/firewall-cmd"

PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/firewalld.log" sh "$script" install
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/firewalld.log" sh "$script" remove

grep -qx -- '--quiet --permanent --add-port=47993/udp' "$base/firewalld.log"
grep -qx -- '--quiet --add-port=47993/udp' "$base/firewalld.log"
grep -qx -- '--quiet --permanent --add-port=47994/udp' "$base/firewalld.log"
grep -qx -- '--quiet --add-port=47994/udp' "$base/firewalld.log"
grep -qx -- '--quiet --permanent --remove-port=47994/udp' "$base/firewalld.log"
grep -qx -- '--quiet --remove-port=47994/udp' "$base/firewalld.log"
grep -qx -- '--quiet --permanent --remove-port=47993/udp' "$base/firewalld.log"
grep -qx -- '--quiet --remove-port=47993/udp' "$base/firewalld.log"

cat >"$base/bin/ufw" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "status" ]; then
  echo "Status: ${OPAL_TEST_UFW_STATUS:-active}"
  exit 0
fi
printf '%s\n' "$*" >>"${OPAL_TEST_FIREWALL_LOG:?}"
EOF
chmod +x "$base/bin/ufw"

: >"$base/both.log"
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/both.log" sh "$script" install
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/both.log" sh "$script" remove

grep -qx -- '--quiet --permanent --add-port=47993/udp' "$base/both.log"
grep -qx -- '--quiet --permanent --add-port=47994/udp' "$base/both.log"
grep -qx -- 'allow 47993/udp comment OPAL LAN discovery' "$base/both.log"
grep -qx -- 'allow 47994/udp comment OPAL LAN discovery replies' "$base/both.log"
grep -qx -- '--force delete allow 47994/udp' "$base/both.log"
grep -qx -- '--force delete allow 47993/udp' "$base/both.log"
test "$(grep -c '^reload$' "$base/both.log")" -eq 2

: >"$base/both-inactive.log"
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_UFW_STATUS=inactive OPAL_TEST_FIREWALL_LOG="$base/both-inactive.log" sh "$script" install

grep -qx -- '--quiet --permanent --add-port=47993/udp' "$base/both-inactive.log"
grep -qx -- '--quiet --permanent --add-port=47994/udp' "$base/both-inactive.log"
grep -qx -- 'allow 47993/udp comment OPAL LAN discovery' "$base/both-inactive.log"
grep -qx -- 'allow 47994/udp comment OPAL LAN discovery replies' "$base/both-inactive.log"
! grep -q '^reload$' "$base/both-inactive.log"

echo 'firewall lifecycle tests passed'
