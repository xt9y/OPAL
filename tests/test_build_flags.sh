#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/bin"

cat >"$TMP/bin/pkg-config" <<'EOF'
#!/bin/sh
case "$1" in
  --cflags)
    echo '-I/opt/opal-pkgconfig-test'
    ;;
  --libs)
    echo '-L/opt/opal-pkgconfig-test -lopal-pkgconfig-test'
    ;;
  --exists)
    exit 0
    ;;
  *)
    exit 0
    ;;
esac
EOF
chmod +x "$TMP/bin/pkg-config"

PATH="$TMP/bin:$PATH" PKG_CONFIG="$TMP/bin/pkg-config" make -C "$ROOT" -n build/opal >"$TMP/out"

grep -q -- '-I/opt/opal-pkgconfig-test' "$TMP/out"
grep -q -- '-L/opt/opal-pkgconfig-test' "$TMP/out"

echo 'build flag tests passed'
