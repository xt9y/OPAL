#!/bin/sh
set -eu
: "${BIN:?}"

base="$(mktemp -d)"
trap 'test -n "${access_pid:-}" && kill "$access_pid" 2>/dev/null || true; test -n "${share_pid:-}" && kill "$share_pid" 2>/dev/null || true; rm -rf "$base"' EXIT

home="$base/home"
opal_home="$base/opal"
bin="$base/bin"
mkdir -p "$home/.zrok2" "$opal_home" "$bin"
printf 'still-enabled\n' > "$home/.zrok2/environment"

cat > "$opal_home/host.ini" <<'EOF'
[tunnel]
control_token=opal-ctl-clean-test
video_token=opal-vid-clean-test
mode=zrok2-private
EOF
cat > "$opal_home/hosts.ini" <<'EOF'
[desktop]
address=opal:remote-control,remote-video

[lan-only]
address=192.0.2.10
EOF
printf 'identity\n' > "$opal_home/identity.key"

cat > "$bin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$SYSTEMCTL_TEST_LOG"
exit 0
EOF
chmod +x "$bin/systemctl"

cat > "$bin/zrok2" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$ZROK_TEST_LOG"
case "$1 $2" in
  'access private'|'share private')
    while :; do sleep 1; done
    ;;
  'list accesses')
    token=''
    prev=''
    for arg in "$@"; do
      if [ "$prev" = --share-token ]; then token="$arg"; break; fi
      prev="$arg"
    done
    case "$token" in
      opal-ctl-clean-test) front='front-host-control' ;;
      opal-vid-clean-test) front='front-host-video' ;;
      remote-control) front='front-remote-control' ;;
      remote-video) front='front-remote-video' ;;
      *) printf '{"accesses":[]}\n'; exit 0 ;;
    esac
    printf '{"accesses":[{"frontendToken":"%s","shareToken":"%s"}]}\n' "$front" "$token"
    exit 0
    ;;
  'list shares')
    token=''
    prev=''
    for arg in "$@"; do
      if [ "$prev" = --share-token ]; then token="$arg"; break; fi
      prev="$arg"
    done
    if [ -n "${ZROK_TEST_STICKY_SHARE:-}" ] && [ "$token" = "$ZROK_TEST_STICKY_SHARE" ]; then
      printf '{"shares":[{"shareToken":"%s"}]}\n' "$token"
    else
      printf '{"shares":[]}\n'
    fi
    exit 0
    ;;
  'delete access')
    exit 0
    ;;
  'delete share')
    if [ -n "${ZROK_TEST_STICKY_SHARE:-}" ] && [ "$3" = "$ZROK_TEST_STICKY_SHARE" ]; then exit 1; fi
    exit 0
    ;;
esac
exit 0
EOF
chmod +x "$bin/zrok2"

export HOME="$home"
export OPAL_HOME="$opal_home"
export PATH="$bin:$PATH"
export SYSTEMCTL_TEST_LOG="$base/systemctl.log"
export ZROK_TEST_LOG="$base/zrok.log"

zrok2 access private stale-client --bind 127.0.0.1:47990 --headless & access_pid=$!
zrok2 share private --headless --share-token stale-host 127.0.0.1:47991 & share_pid=$!
sleep 0.2
kill -0 "$access_pid"
kill -0 "$share_pid"

"$BIN" clean > "$base/clean.out"

if kill -0 "$access_pid" 2>/dev/null; then
    echo 'stale zrok access process survived opal clean' >&2
    exit 1
fi
access_pid=""
if kill -0 "$share_pid" 2>/dev/null; then
    echo 'stale zrok share process survived opal clean' >&2
    exit 1
fi
share_pid=""

test ! -e "$opal_home"
test -f "$home/.zrok2/environment"
for token in opal-ctl-clean-test opal-vid-clean-test remote-control remote-video; do
    grep -q "^list accesses --share-token $token --json$" "$base/zrok.log"
done
for token in opal-ctl-clean-test opal-vid-clean-test; do
    grep -q "^list shares --share-token $token --json$" "$base/zrok.log"
done
for frontend in front-host-control front-host-video front-remote-control front-remote-video; do
    grep -q "^delete access $frontend$" "$base/zrok.log"
done
grep -q '^delete share opal-ctl-clean-test$' "$base/zrok.log"
grep -q '^delete share opal-vid-clean-test$' "$base/zrok.log"
! grep -q '^delete share remote-control$' "$base/zrok.log"
! grep -q '^delete share remote-video$' "$base/zrok.log"
! grep -q '^disable' "$base/zrok.log"
grep -q '^--user disable --now opal-host.service$' "$base/systemctl.log"
grep -q '^--user disable --now opal-bridge.service$' "$base/systemctl.log"
grep -q 'OPAL state cleaned' "$base/clean.out"

# A controller-side share that survives deletion must make clean fail and keep
# the OPAL token inventory so the command can be retried later.
opal_home="$base/opal-sticky"
mkdir -p "$opal_home"
cat > "$opal_home/host.ini" <<'EOF'
[tunnel]
control_token=opal-ctl-sticky
video_token=opal-vid-sticky
mode=zrok2-private
EOF
: > "$base/zrok-sticky.log"
export OPAL_HOME="$opal_home"
export ZROK_TEST_LOG="$base/zrok-sticky.log"
export ZROK_TEST_STICKY_SHARE=opal-ctl-sticky
set +e
"$BIN" clean > "$base/clean-sticky.out" 2> "$base/clean-sticky.err"
rc=$?
set -e
test "$rc" -eq 1
test -d "$opal_home"
test -f "$opal_home/host.ini"
grep -q '^delete share opal-ctl-sticky$' "$base/zrok-sticky.log"
grep -q '^list shares --share-token opal-ctl-sticky --json$' "$base/zrok-sticky.log"
grep -q 'cleanup incomplete' "$base/clean-sticky.err"
unset ZROK_TEST_STICKY_SHARE

echo 'clean tests passed'
