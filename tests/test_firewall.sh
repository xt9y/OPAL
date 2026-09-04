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
grep -qx -- '--quiet --permanent --add-rich-rule=rule source-port port="47993" protocol="udp" accept' "$base/firewalld.log"
grep -qx -- '--quiet --add-rich-rule=rule source-port port="47993" protocol="udp" accept' "$base/firewalld.log"
grep -qx -- '--quiet --permanent --remove-rich-rule=rule source-port port="47993" protocol="udp" accept' "$base/firewalld.log"
grep -qx -- '--quiet --remove-rich-rule=rule source-port port="47993" protocol="udp" accept' "$base/firewalld.log"
grep -qx -- '--quiet --permanent --remove-port=47993/udp' "$base/firewalld.log"
grep -qx -- '--quiet --remove-port=47993/udp' "$base/firewalld.log"

rm -f "$base/bin/firewall-cmd"
cat >"$base/bin/ufw" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "status" ]; then
  echo 'Status: active'
  exit 0
fi
printf '%s\n' "$*" >>"${OPAL_TEST_FIREWALL_LOG:?}"
EOF
chmod +x "$base/bin/ufw"

PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/ufw.log" sh "$script" install
PATH="$base/bin:/usr/bin:/bin" OPAL_TEST_FIREWALL_LOG="$base/ufw.log" sh "$script" remove

grep -qx -- 'allow 47993/udp comment OPAL LAN discovery' "$base/ufw.log"
grep -qx -- 'allow in proto udp from any port 47993 to any comment OPAL LAN discovery replies' "$base/ufw.log"
grep -qx -- '--force delete allow in proto udp from any port 47993 to any' "$base/ufw.log"
grep -qx -- '--force delete allow 47993/udp' "$base/ufw.log"
test "$(grep -c '^reload$' "$base/ufw.log")" -eq 2

echo 'firewall lifecycle tests passed'
