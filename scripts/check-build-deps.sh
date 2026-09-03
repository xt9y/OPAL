#!/bin/sh
set -eu

PKG_CONFIG_BIN=${PKG_CONFIG:-pkg-config}
REQUIRED_PKGS='openssl x11 xi gl libpulse-simple libavformat libavcodec libavutil libswresample'

print_install_hint() {
    id=''
    like=''
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        id=${ID:-}
        like=${ID_LIKE:-}
    fi

    case "$id $like" in
        *fedora*|*rhel*)
            cat >&2 <<'EOF'
Install the Fedora / Fedora Asahi build dependencies with:
  sudo dnf install -y gcc-c++ make pkgconf-pkg-config openssl-devel libX11-devel libXi-devel libglvnd-devel pulseaudio-libs-devel ffmpeg-free ffmpeg-free-devel

If this machine intentionally uses RPM Fusion's full FFmpeg packages instead of Fedora ffmpeg-free, use ffmpeg + ffmpeg-devel in place of ffmpeg-free + ffmpeg-free-devel.
EOF
            ;;
        *debian*|*ubuntu*)
            cat >&2 <<'EOF'
Install the Debian / Ubuntu build dependencies with:
  sudo apt-get install -y g++ make pkg-config libssl-dev libx11-dev libxi-dev libgl1-mesa-dev libpulse-dev ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
EOF
            ;;
        *arch*)
            cat >&2 <<'EOF'
Install the Arch / CachyOS build dependencies with:
  sudo pacman -S --needed base-devel pkgconf openssl libx11 libxi libglvnd libpulse ffmpeg
EOF
            ;;
        *)
            cat >&2 <<'EOF'
Install a C++20 compiler, pkg-config, OpenSSL/X11/XInput/GL/PulseAudio development files, and FFmpeg development files providing:
  libavformat libavcodec libavutil libswresample
EOF
            ;;
    esac
}

if ! command -v "$PKG_CONFIG_BIN" >/dev/null 2>&1; then
    echo "OPAL build dependency check failed: pkg-config is not installed." >&2
    print_install_hint
    exit 1
fi

missing=''
for pkg in $REQUIRED_PKGS; do
    if ! "$PKG_CONFIG_BIN" --exists "$pkg"; then
        missing="$missing $pkg"
    fi
done

if [ -n "$missing" ]; then
    echo "OPAL build dependency check failed." >&2
    echo "Missing pkg-config modules:$missing" >&2
    print_install_hint
    exit 1
fi
