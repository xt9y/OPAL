#!/bin/sh
set -eu
: "${MAKE:=make}"
# A real install must apply uinput access immediately, without requiring reboot/logout.
base="$(mktemp -d)"
trap 'rm -rf "$base"' EXIT
mkdir -p "$base/bin" "$base/prefix" "$base/udev"
cat >"$base/bin/modprobe" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"${OPAL_TEST_MODPROBE_LOG:?}"
EOF
cat >"$base/bin/udevadm" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"${OPAL_TEST_UDEV_LOG:?}"
EOF
chmod +x "$base/bin/modprobe" "$base/bin/udevadm"

PATH="$base/bin:$PATH" \
OPAL_SKIP_FIREWALL=1 \
OPAL_TEST_MODPROBE_LOG="$base/modprobe.log" \
OPAL_TEST_UDEV_LOG="$base/udev.log" \
"$MAKE" install PREFIX="$base/prefix" UDEVDIR="$base/udev" >/dev/null

test -f "$base/udev/70-opal-uinput.rules"
grep -q 'KERNEL=="uinput"' "$base/udev/70-opal-uinput.rules"
grep -q 'TAG+="uaccess"' "$base/udev/70-opal-uinput.rules"
grep -qx 'uinput' "$base/modprobe.log"
grep -qx 'control --reload-rules' "$base/udev.log"
grep -q '^trigger .*uinput' "$base/udev.log"
echo 'uinput install lifecycle tests passed'
