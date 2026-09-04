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

rm -f "$base/bin/firewall-cmd"
cat >"$base/bin/ufw" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "status" ]; then
  echo "Status: ${OPAL_TEST_UFW_STATUS:-active}"
  exit 0
fi
printf '%s\n' "$*" >>"${OPAL_TEST_FIREWALL_LOG:?}"
EOF
chmod +x "$base/bin/ufw"

PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/ufw.log" sh "$script" install
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/ufw.log" sh "$script" remove

grep -qx -- 'allow 47993/udp comment OPAL LAN discovery' "$base/ufw.log"
grep -qx -- 'allow 47994/udp comment OPAL LAN discovery replies' "$base/ufw.log"
grep -qx -- '--force delete allow 47994/udp' "$base/ufw.log"
grep -qx -- '--force delete allow 47993/udp' "$base/ufw.log"
test "$(grep -c '^reload$' "$base/ufw.log")" -eq 2

: >"$base/ufw-inactive.log"
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_UFW_STATUS=inactive OPAL_TEST_FIREWALL_LOG="$base/ufw-inactive.log" sh "$script" install

grep -qx -- 'allow 47993/udp comment OPAL LAN discovery' "$base/ufw-inactive.log"
grep -qx -- 'allow 47994/udp comment OPAL LAN discovery replies' "$base/ufw-inactive.log"
! grep -q '^reload$' "$base/ufw-inactive.log"

echo 'firewall lifecycle tests passed'
