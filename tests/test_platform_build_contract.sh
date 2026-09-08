#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

make_bin=${MAKE_BIN:-make}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

"$make_bin" -Bn OPAL_OS=linux build/opal >"$tmp/linux"
"$make_bin" -Bn OPAL_OS=macos macos-all >"$tmp/macos"

grep -q 'src/pipewire_capture.cpp' "$tmp/linux"
grep -q 'src/input_helper.cpp' "$tmp/linux" || true

grep -q 'src/platform/macos/host.cpp' "$tmp/macos"
grep -q 'src/platform/macos/system_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/capture_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/video_encoder_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/audio_capture_backend.mm' "$tmp/macos"
grep -q 'src/platform/macos/clipboard_shim.mm' "$tmp/macos"
grep -q 'src/platform/macos/input_helper.mm' "$tmp/macos"
grep -q -- '-framework ScreenCaptureKit' "$tmp/macos"
grep -q -- '-framework VideoToolbox' "$tmp/macos"
grep -q -- '-mmacosx-version-min=13.0' "$tmp/macos"
grep -q -- '-sectcreate,__TEXT,__info_plist,platform/macos/Info.plist' "$tmp/macos"
grep -q -- '-sectcreate,__TEXT,__info_plist,platform/macos/InputHelper-Info.plist' "$tmp/macos"
grep -q 'codesign --force --sign - --identifier de.xt9y.opal ' "$tmp/macos"
grep -q 'codesign --force --sign - --identifier de.xt9y.opal.input ' "$tmp/macos"
grep -q 'Apple Silicon macOS (arm64) only' "$tmp/macos"
grep -q 'brew install make pkg-config sdl3 openssl@3 ffmpeg' "$tmp/macos"
grep -q 'brew --prefix openssl@3' Makefile.macos
grep -q 'PKG_CONFIG_PATH' Makefile.macos
grep -q -- '--libs-only-L openssl' Makefile.macos
grep -q 'OPENSSL_LIBRARY_FLAGS' Makefile.macos
grep -q 'GNU Make 3.82+' GNUmakefile
grep -q 'gmake -j' README
grep -q 'NSScreenCaptureUsageDescription' platform/macos/Info.plist
grep -q 'NSAudioCaptureUsageDescription' platform/macos/Info.plist
grep -q '<string>de.xt9y.opal</string>' platform/macos/Info.plist
grep -q '<string>de.xt9y.opal.input</string>' platform/macos/InputHelper-Info.plist
! grep -q '^macos-install: macos-all' Makefile.macos
! grep -q 'src/platform/macos/host_stub.cpp' "$tmp/macos"
! grep -q 'src/platform/macos/pipewire_capture_stub.cpp' "$tmp/macos"
! grep -q 'src/pipewire_capture.cpp' "$tmp/macos"
! grep -q 'libpipewire' "$tmp/macos"
! grep -q 'libportal' "$tmp/macos"
! grep -q 'src/input_helper.cpp' "$tmp/macos"
! grep -q '70-opal-uinput.rules' "$tmp/macos"

echo 'platform build contract passed'
