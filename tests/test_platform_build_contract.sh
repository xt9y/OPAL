#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

make -Bn OPAL_OS=linux build/opal >"$tmp/linux"
make -Bn OPAL_OS=macos macos-all >"$tmp/macos"

grep -q 'src/pipewire_capture.cpp' "$tmp/linux"
grep -q 'src/input_helper.cpp' "$tmp/linux" || true

grep -q 'src/host.cpp' "$tmp/macos"
grep -q 'src/platform/macos/system_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/capture_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/video_encoder_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/audio_capture_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/clipboard_shim.mm' "$tmp/macos"
grep -q 'src/platform/macos/input_helper.mm' "$tmp/macos"
grep -q -- '-framework ScreenCaptureKit' "$tmp/macos"
grep -q -- '-framework VideoToolbox' "$tmp/macos"
! grep -q 'src/platform/macos/host_stub.cpp' "$tmp/macos"
! grep -q 'src/platform/macos/pipewire_capture_stub.cpp' "$tmp/macos"
! grep -q 'src/pipewire_capture.cpp' "$tmp/macos"
! grep -q 'libpipewire' "$tmp/macos"
! grep -q 'libportal' "$tmp/macos"
! grep -q 'src/input_helper.cpp' "$tmp/macos"
! grep -q '70-opal-uinput.rules' "$tmp/macos"

echo 'platform build contract passed'
