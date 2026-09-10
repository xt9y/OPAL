# OPAL unified build
# Public targets: make, make install, make uninstall, make clean, make verify

UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
OPAL_OS ?= $(if $(filter Windows_NT,$(OS)),windows,$(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),windows,$(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))))

.DEFAULT_GOAL := all

CC ?= cc
CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config
INSTALL ?= install
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec/opal
SYSTEMDUSERDIR ?= $(PREFIX)/lib/systemd/user
UDEVDIR ?= /usr/lib/udev/rules.d
APPLICATIONSDIR ?= $(PREFIX)/share/applications
PUBLIC_HOST ?= rendezvous.opal.xt9y.de
RENDEZVOUS_PORT ?= 47992
BUILD := build

ifeq ($(OPAL_OS),windows)
PRODUCT := $(BUILD)/opal.exe
INPUT := $(BUILD)/opal-input.exe
else
PRODUCT := $(BUILD)/opal
INPUT := $(BUILD)/opal-input
endif

RENDEZVOUS_SERVER := $(BUILD)/opal-rendezvous

ifeq ($(OPAL_OS),macos)
HOMEBREW_OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
ORIGINAL_PKG_CONFIG_PATH := $(PKG_CONFIG_PATH)
ifneq ($(strip $(HOMEBREW_OPENSSL_PREFIX)),)
PKG_CONFIG_PATH := $(HOMEBREW_OPENSSL_PREFIX)/lib/pkgconfig$(if $(strip $(ORIGINAL_PKG_CONFIG_PATH)),:$(ORIGINAL_PKG_CONFIG_PATH))
export PKG_CONFIG_PATH
endif
endif

AV_PKGS := libavformat libavcodec libavutil
FFMPEG_PKGS := $(AV_PKGS) libswresample
FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PKGS) 2>/dev/null)
SDL3_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL3_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
AVLIBS := $(shell $(PKG_CONFIG) --libs $(AV_PKGS) 2>/dev/null)
SWRLIBS := $(shell $(PKG_CONFIG) --libs libswresample 2>/dev/null)
CPPFLAGS += -Iinclude $(FFMPEG_CFLAGS) $(SDL3_CFLAGS)

PROFILE_SRCS := src/media_profile.cpp
VIDEO_CRYPTO_SRCS := src/video_crypto.cpp
VIDEO_PACKET_SRCS := src/video_packet.cpp
VIDEO_REASSEMBLY_SRCS := src/video_reassembly.cpp
VIDEO_FEEDBACK_SRCS := src/video_feedback.cpp
AUDIO_OUTPUT_SRCS := src/audio_output.cpp
VIDEO_SENDER_SRCS := src/video_sender.cpp
VIDEO_RECEIVER_SRCS := src/video_receiver.cpp
RENDEZVOUS_PROTOCOL_SRCS := src/rendezvous_protocol.cpp
RENDEZVOUS_STATE_SRCS := src/rendezvous_server.cpp
RENDEZVOUS_CLIENT_SRCS := src/rendezvous_client.cpp
RELAY_SRCS := src/relay_protocol.cpp
PEER_HANDSHAKE_SRCS := src/peer_handshake.cpp
SESSION_PACKET_SRCS := src/session_packet.cpp
RELIABLE_CONTROL_SRCS := src/reliable_control.cpp
PEER_SESSION_SRCS := src/peer_session.cpp
INPUT_SRCS := src/input.cpp

CONTROL_COMMON_SRCS := \
	$(RENDEZVOUS_PROTOCOL_SRCS) \
	$(RENDEZVOUS_CLIENT_SRCS) \
	$(RELAY_SRCS) \
	$(PEER_HANDSHAKE_SRCS) \
	$(SESSION_PACKET_SRCS) \
	$(RELIABLE_CONTROL_SRCS) \
	$(PEER_SESSION_SRCS)

$(BUILD):
	@mkdir -p "$(BUILD)"

ifeq ($(OPAL_OS),linux)

CXXFLAGS += -pthread
NATIVE_CAPTURE_PKGS := libportal libpipewire-0.3 libswscale wayland-client
NATIVE_CAPTURE_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(NATIVE_CAPTURE_PKGS) 2>/dev/null)
NATIVE_CAPTURE_LIBS := $(shell $(PKG_CONFIG) --libs $(NATIVE_CAPTURE_PKGS) 2>/dev/null)
WAYLAND_CLIENT_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client 2>/dev/null)
CPPFLAGS += $(NATIVE_CAPTURE_CFLAGS) -I$(BUILD) -DOPAL_HAVE_NATIVE_PIPEWIRE=1

LINUX_PROTOCOL_XML := platform/linux/protocols/zkde-screencast-unstable-v1.xml
LINUX_PROTOCOL_HEADER := $(BUILD)/zkde-screencast-client-protocol.h
LINUX_PROTOCOL_CODE := $(BUILD)/zkde-screencast-protocol.c
LINUX_PROTOCOL_OBJ := $(BUILD)/zkde-screencast-protocol.o
LINUX_FAKE_INPUT_XML := platform/linux/protocols/fake-input.xml
LINUX_FAKE_INPUT_HEADER := $(BUILD)/fake-input-client-protocol.h
LINUX_FAKE_INPUT_CODE := $(BUILD)/fake-input-protocol.c
LINUX_FAKE_INPUT_OBJ := $(BUILD)/fake-input-protocol.o

LINUX_HEADLESS_SRCS := \
	src/host_display.cpp \
	src/platform/linux/display_backend.cpp \
	src/platform/linux/headless_session.cpp \
	src/platform/linux/kwin_virtual_display.cpp \
	src/platform/linux/capture_backend.cpp \
	src/platform/linux/video_encoder_backend.cpp \
	src/native_video_pipeline.cpp

LINUX_CAPTURE_SRCS := src/video_capture.cpp src/pipewire_capture.cpp src/flv_stream.cpp $(LINUX_HEADLESS_SRCS)
LINUX_APP_SRCS := \
	src/main.cpp src/setup.cpp src/host.cpp src/client.cpp src/session.cpp src/system.cpp \
	src/config.cpp src/crypto.cpp src/media.cpp src/wake.cpp \
	$(PROFILE_SRCS) \
	src/udp_transport.cpp $(VIDEO_CRYPTO_SRCS) $(VIDEO_PACKET_SRCS) $(VIDEO_FEEDBACK_SRCS) \
	$(VIDEO_RECEIVER_SRCS) $(VIDEO_REASSEMBLY_SRCS) src/video_decoder.cpp $(AUDIO_OUTPUT_SRCS) \
	$(VIDEO_SENDER_SRCS) $(LINUX_CAPTURE_SRCS) src/video_present.cpp \
	$(CONTROL_COMMON_SRCS) src/local_discovery.cpp src/udp_socket_ops.cpp \
	$(INPUT_SRCS) src/clipboard.cpp src/tailnet.cpp

LINUX_LIBS := -lcrypto -lpthread $(AVLIBS) $(NATIVE_CAPTURE_LIBS) $(SWRLIBS) $(SDL3_LIBS)

$(LINUX_PROTOCOL_HEADER): $(LINUX_PROTOCOL_XML) | $(BUILD)
	wayland-scanner client-header $< $@

$(LINUX_PROTOCOL_CODE): $(LINUX_PROTOCOL_XML) | $(BUILD)
	wayland-scanner private-code $< $@

$(LINUX_PROTOCOL_OBJ): $(LINUX_PROTOCOL_CODE) $(LINUX_PROTOCOL_HEADER)
	$(CC) $(NATIVE_CAPTURE_CFLAGS) -I$(BUILD) -c $(LINUX_PROTOCOL_CODE) -o $@

$(LINUX_FAKE_INPUT_HEADER): $(LINUX_FAKE_INPUT_XML) | $(BUILD)
	wayland-scanner client-header $< $@

$(LINUX_FAKE_INPUT_CODE): $(LINUX_FAKE_INPUT_XML) | $(BUILD)
	wayland-scanner private-code $< $@

$(LINUX_FAKE_INPUT_OBJ): $(LINUX_FAKE_INPUT_CODE) $(LINUX_FAKE_INPUT_HEADER)
	$(CC) $(NATIVE_CAPTURE_CFLAGS) -I$(BUILD) -c $(LINUX_FAKE_INPUT_CODE) -o $@

deps-check:
	@set -e; \
	command -v "$(PKG_CONFIG)" >/dev/null 2>&1 || { echo 'Missing pkg-config.' >&2; exit 1; }; \
	command -v wayland-scanner >/dev/null 2>&1 || { echo 'Missing wayland-scanner (wayland package).' >&2; exit 1; }; \
	missing=''; \
	for pkg in openssl sdl3 libavformat libavcodec libavutil libswresample $(NATIVE_CAPTURE_PKGS); do \
		if ! "$(PKG_CONFIG)" --exists "$$pkg"; then missing="$$missing $$pkg"; fi; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Missing Linux build modules:$$missing" >&2; \
		echo 'Install SDL3, OpenSSL, FFmpeg, PipeWire, libportal and Wayland development packages.' >&2; \
		exit 1; \
	fi

$(PRODUCT): $(LINUX_APP_SRCS) $(LINUX_PROTOCOL_OBJ) include/opal/*.hpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINUX_APP_SRCS) $(LINUX_PROTOCOL_OBJ) $(LDFLAGS) $(LINUX_LIBS) -o $@

$(INPUT): src/input_helper.cpp include/opal/input_record.hpp include/opal/input_wire.hpp $(LINUX_FAKE_INPUT_OBJ) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/input_helper.cpp $(LINUX_FAKE_INPUT_OBJ) $(WAYLAND_CLIENT_LIBS) -o $@

all: $(PRODUCT) $(INPUT)

install: all
	@set -e; \
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBEXECDIR)" "$(DESTDIR)$(SYSTEMDUSERDIR)" "$(DESTDIR)$(UDEVDIR)" "$(DESTDIR)$(APPLICATIONSDIR)"; \
	$(INSTALL) -m 0755 "$(PRODUCT)" "$(DESTDIR)$(BINDIR)/opal"; \
	$(INSTALL) -m 0755 "$(INPUT)" "$(DESTDIR)$(LIBEXECDIR)/opal-input"; \
	$(INSTALL) -m 0644 system/opal-host.service "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-host.service"; \
	$(INSTALL) -m 0644 system/opal-bridge.service "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-bridge.service"; \
	$(INSTALL) -m 0644 system/70-opal-uinput.rules "$(DESTDIR)$(UDEVDIR)/70-opal-uinput.rules"; \
	$(INSTALL) -m 0644 system/de.xt9y.opal.desktop "$(DESTDIR)$(APPLICATIONSDIR)/de.xt9y.opal.desktop"; \
	$(INSTALL) -m 0644 system/de.xt9y.opal.input.desktop "$(DESTDIR)$(APPLICATIONSDIR)/de.xt9y.opal.input.desktop"; \
	if [ -z "$(DESTDIR)" ]; then \
		$(MAKE) --no-print-directory firewall-install; \
		if command -v modprobe >/dev/null 2>&1; then modprobe uinput || true; fi; \
		if command -v udevadm >/dev/null 2>&1; then \
			udevadm control --reload-rules; \
			udevadm trigger --action=change --sysname-match=uinput; \
			udevadm settle; \
		fi; \
		login_user="$${SUDO_USER:-$${USER:-}}"; \
		if command -v loginctl >/dev/null 2>&1 && [ -n "$$login_user" ] && [ "$$login_user" != root ]; then \
			if [ "$$(id -u)" -eq 0 ]; then loginctl enable-linger "$$login_user" >/dev/null 2>&1 || true; \
			elif command -v sudo >/dev/null 2>&1; then sudo loginctl enable-linger "$$login_user" >/dev/null 2>&1 || true; fi; \
		fi; \
	fi; \
	echo "Installed OPAL to $(DESTDIR)$(PREFIX)"

uninstall:
	@set -e; \
	if [ -z "$(DESTDIR)" ]; then $(MAKE) --no-print-directory firewall-remove; fi; \
	rm -f "$(DESTDIR)$(BINDIR)/opal" "$(DESTDIR)$(BINDIR)/opal-rendezvous" "$(DESTDIR)$(LIBEXECDIR)/opal-input"; \
	rm -f "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-host.service" "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-bridge.service"; \
	rm -f "$(DESTDIR)$(APPLICATIONSDIR)/de.xt9y.opal.desktop" "$(DESTDIR)$(APPLICATIONSDIR)/de.xt9y.opal.input.desktop"; \
	rmdir "$(DESTDIR)$(LIBEXECDIR)" 2>/dev/null || true

firewall-install:
	@if [ "$${OPAL_SKIP_FIREWALL:-0}" = 1 ]; then exit 0; fi; \
	discovery_rule='47993/udp'; reply_rule='47994/udp'; \
	if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then \
		firewall-cmd --quiet --permanent --add-port="$$discovery_rule"; \
		firewall-cmd --quiet --add-port="$$discovery_rule"; \
		firewall-cmd --quiet --permanent --add-port="$$reply_rule"; \
		firewall-cmd --quiet --add-port="$$reply_rule"; \
	fi; \
	if command -v ufw >/dev/null 2>&1; then \
		ufw_active=0; \
		if LC_ALL=C ufw status 2>/dev/null | grep -q '^Status: active'; then ufw_active=1; fi; \
		ufw allow "$$discovery_rule" comment 'OPAL LAN discovery'; \
		ufw allow "$$reply_rule" comment 'OPAL LAN discovery replies'; \
		if [ "$$ufw_active" -eq 1 ]; then ufw reload >/dev/null; fi; \
	fi

firewall-remove:
	@if [ "$${OPAL_SKIP_FIREWALL:-0}" = 1 ]; then exit 0; fi; \
	discovery_rule='47993/udp'; reply_rule='47994/udp'; \
	if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then \
		firewall-cmd --quiet --permanent --remove-port="$$reply_rule" >/dev/null 2>&1 || true; \
		firewall-cmd --quiet --remove-port="$$reply_rule" >/dev/null 2>&1 || true; \
		firewall-cmd --quiet --permanent --remove-port="$$discovery_rule" >/dev/null 2>&1 || true; \
		firewall-cmd --quiet --remove-port="$$discovery_rule" >/dev/null 2>&1 || true; \
	fi; \
	if command -v ufw >/dev/null 2>&1; then \
		ufw_active=0; \
		if LC_ALL=C ufw status 2>/dev/null | grep -q '^Status: active'; then ufw_active=1; fi; \
		ufw --force delete allow "$$reply_rule" >/dev/null 2>&1 || true; \
		ufw --force delete allow "$$discovery_rule" >/dev/null 2>&1 || true; \
		if [ "$$ufw_active" -eq 1 ]; then ufw reload >/dev/null 2>&1 || true; fi; \
	fi

RENDEZVOUS_LIBS := -lcrypto -lpthread

else ifeq ($(OPAL_OS),macos)

MACOSX_DEPLOYMENT_TARGET ?= 13.0
export MACOSX_DEPLOYMENT_TARGET
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)
OPENSSL_LIBRARY_FLAGS := $(shell $(PKG_CONFIG) --libs-only-L openssl 2>/dev/null)
CXXFLAGS += -pthread -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) $(OPENSSL_LIBRARY_FLAGS)
CPPFLAGS += $(OPENSSL_CFLAGS) -DOPAL_PLATFORM_MACOS=1 \
	-DSDL_GetClipboardText=opal_macos_get_clipboard_text \
	-DSDL_SetClipboardText=opal_macos_set_clipboard_text

APPLE_FRAMEWORKS := \
	-framework Foundation \
	-framework AppKit \
	-framework CoreGraphics \
	-framework ApplicationServices \
	-framework ScreenCaptureKit \
	-framework CoreMedia \
	-framework CoreVideo \
	-framework VideoToolbox \
	-framework AudioToolbox

MACOS_INFO_PLIST := platform/macos/Info.plist
MACOS_INPUT_INFO_PLIST := platform/macos/InputHelper-Info.plist
MACOS_INFO_LDFLAGS := -Wl,-sectcreate,__TEXT,__info_plist,$(MACOS_INFO_PLIST)
MACOS_INPUT_INFO_LDFLAGS := -Wl,-sectcreate,__TEXT,__info_plist,$(MACOS_INPUT_INFO_PLIST)

MACOS_VIDEO_SRCS := \
	src/host_display.cpp \
	src/native_video_capture.cpp \
	src/native_video_pipeline.cpp \
	src/platform/macos/display_backend.mm \
	src/platform/macos/capture_backend.mm \
	src/platform/macos/video_encoder_backend.mm \
	src/platform/macos/audio_capture_backend.mm

MACOS_APP_SRCS := \
	src/main.cpp src/setup.cpp src/platform/macos/host.cpp src/client.cpp src/session.cpp \
	src/platform/macos/system_backend.mm \
	src/config.cpp src/crypto.cpp src/media.cpp src/wake.cpp \
	$(PROFILE_SRCS) \
	src/udp_transport.cpp $(VIDEO_CRYPTO_SRCS) $(VIDEO_PACKET_SRCS) $(VIDEO_FEEDBACK_SRCS) \
	$(VIDEO_RECEIVER_SRCS) $(VIDEO_REASSEMBLY_SRCS) src/video_decoder.cpp $(AUDIO_OUTPUT_SRCS) \
	$(VIDEO_SENDER_SRCS) $(MACOS_VIDEO_SRCS) src/video_present.cpp \
	$(CONTROL_COMMON_SRCS) src/local_discovery.cpp src/udp_socket_ops.cpp \
	$(INPUT_SRCS) src/clipboard.cpp src/platform/macos/clipboard_shim.mm src/tailnet.cpp

MACOS_FFMPEG_LIBS := $(AVLIBS) $(SWRLIBS)
MACOS_LIBS := $(OPENSSL_LIBS) -pthread $(MACOS_FFMPEG_LIBS) $(SDL3_LIBS) $(APPLE_FRAMEWORKS)

deps-check:
	@set -e; \
	os=$$(uname -s); arch=$$(uname -m); \
	[ "$$os" = Darwin ] || { echo 'OPAL macOS build must run on macOS.' >&2; exit 1; }; \
	[ "$$arch" = arm64 ] || { echo 'OPAL currently supports Apple Silicon macOS (arm64) only.' >&2; exit 1; }; \
	command -v brew >/dev/null 2>&1 || { echo 'Homebrew is required. Install: brew install pkg-config sdl3 openssl@3 ffmpeg' >&2; exit 1; }; \
	command -v "$(PKG_CONFIG)" >/dev/null 2>&1 || { echo 'Missing pkg-config. Install: brew install pkg-config' >&2; exit 1; }; \
	command -v codesign >/dev/null 2>&1 || { echo 'Missing codesign. Install Xcode Command Line Tools.' >&2; exit 1; }; \
	missing=''; \
	for pkg in openssl sdl3 libavformat libavcodec libavutil libswresample; do \
		if ! "$(PKG_CONFIG)" --exists "$$pkg"; then missing="$$missing $$pkg"; fi; \
	done; \
	if [ -n "$$missing" ]; then echo "Missing macOS build modules:$$missing" >&2; exit 1; fi

$(PRODUCT): $(MACOS_APP_SRCS) include/opal/*.hpp $(MACOS_INFO_PLIST) | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MACOS_APP_SRCS) $(LDFLAGS) $(MACOS_INFO_LDFLAGS) $(MACOS_LIBS) -o $@

$(INPUT): src/platform/macos/input_helper.mm include/opal/input_record.hpp include/opal/input_wire.hpp $(MACOS_INPUT_INFO_PLIST) | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fobjc-arc src/platform/macos/input_helper.mm $(MACOS_INPUT_INFO_LDFLAGS) -framework ApplicationServices -framework CoreGraphics -o $@

macos-sign: $(PRODUCT) $(INPUT)
	codesign --force --sign - --identifier de.xt9y.opal "$(PRODUCT)"
	codesign --force --sign - --identifier de.xt9y.opal.input "$(INPUT)"
	codesign --verify --strict "$(PRODUCT)"
	codesign --verify --strict "$(INPUT)"

all: macos-sign

install: all
	@if [ "$$(id -u)" -eq 0 ]; then echo 'Run make install as a normal user; it invokes sudo only for final installation.' >&2; exit 2; fi
	@set -e; \
	if [ -n "$(DESTDIR)" ]; then \
		$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBEXECDIR)"; \
		$(INSTALL) -m 0755 "$(PRODUCT)" "$(DESTDIR)$(BINDIR)/opal"; \
		$(INSTALL) -m 0755 "$(INPUT)" "$(DESTDIR)$(LIBEXECDIR)/opal-input"; \
	else \
		command -v sudo >/dev/null 2>&1 || { echo 'sudo is required to install under $(PREFIX)' >&2; exit 1; }; \
		sudo $(INSTALL) -d "$(BINDIR)" "$(LIBEXECDIR)"; \
		sudo $(INSTALL) -m 0755 "$(PRODUCT)" "$(BINDIR)/opal"; \
		sudo $(INSTALL) -m 0755 "$(INPUT)" "$(LIBEXECDIR)/opal-input"; \
	fi; \
	echo 'Installed OPAL.'

uninstall:
	@set -e; \
	if [ -n "$(DESTDIR)" ]; then \
		rm -f "$(DESTDIR)$(BINDIR)/opal" "$(DESTDIR)$(LIBEXECDIR)/opal-input"; \
		rmdir "$(DESTDIR)$(LIBEXECDIR)" 2>/dev/null || true; \
	else \
		command -v sudo >/dev/null 2>&1 || { echo 'sudo is required to uninstall from $(PREFIX)' >&2; exit 1; }; \
		sudo rm -f "$(BINDIR)/opal" "$(LIBEXECDIR)/opal-input"; \
		sudo rmdir "$(LIBEXECDIR)" 2>/dev/null || true; \
	fi

RENDEZVOUS_LIBS := $(OPENSSL_LIBS) -pthread

else ifeq ($(OPAL_OS),windows)

CPPFLAGS += -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DOPAL_PLATFORM_WINDOWS=1 \
	-DSDL_GetClipboardText=opal_windows_get_clipboard_text \
	-DSDL_SetClipboardText=opal_windows_set_clipboard_text
CXXFLAGS += -pthread
WINDOWS_BINDIR ?= $(if $(strip $(MINGW_PREFIX)),$(MINGW_PREFIX)/bin,/ucrt64/bin)
WINDOWS_ARCH := $(shell uname -m 2>/dev/null | tr '[:upper:]' '[:lower:]')
WINDOWS_IDD_PLATFORM ?= $(if $(filter arm64 aarch64,$(WINDOWS_ARCH)),ARM64,x64)
WINDOWS_IDD_PROJECT := platform/windows/idd/OpalDisplay.vcxproj
WINDOWS_IDD_INSTALLER := $(BUILD)/opal-display-install.exe
WINDOWS_IDD_STAMP := $(BUILD)/idd/.built-$(WINDOWS_IDD_PLATFORM)
WINDOWS_MSBUILD ?= $(shell powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '$$cmd=Get-Command MSBuild.exe -ErrorAction SilentlyContinue; if($$cmd){$$cmd.Source; exit}; $$vswhere=Join-Path $${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"; if(Test-Path $$vswhere){& $$vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1}' 2>/dev/null | tr -d '\r')

WINDOWS_VIDEO_SRCS := \
	src/host_display.cpp \
	src/native_video_capture.cpp \
	src/native_video_pipeline.cpp \
	src/platform/windows/display_backend.cpp \
	src/platform/windows/idd_capture_backend.cpp \
	src/platform/windows/idd_status.cpp \
	src/platform/windows/capture_backend.cpp \
	src/platform/windows/video_encoder_backend.cpp \
	src/platform/windows/audio_capture_backend.cpp

WINDOWS_APP_SRCS := \
	src/main.cpp src/setup.cpp src/platform/windows/host.cpp src/client.cpp src/session.cpp \
	src/platform/windows/system_backend.cpp \
	src/config.cpp src/crypto.cpp src/platform/windows/media.cpp src/platform/windows/wake.cpp \
	$(PROFILE_SRCS) \
	src/platform/windows/udp_transport.cpp $(VIDEO_CRYPTO_SRCS) $(VIDEO_PACKET_SRCS) $(VIDEO_FEEDBACK_SRCS) \
	$(VIDEO_RECEIVER_SRCS) $(VIDEO_REASSEMBLY_SRCS) src/platform/windows/video_decoder.cpp $(AUDIO_OUTPUT_SRCS) \
	$(VIDEO_SENDER_SRCS) $(WINDOWS_VIDEO_SRCS) src/platform/windows/video_present.cpp \
	$(CONTROL_COMMON_SRCS) src/local_discovery.cpp src/platform/windows/udp_socket_ops.cpp \
	$(INPUT_SRCS) src/clipboard.cpp src/platform/windows/clipboard_shim.cpp src/platform/windows/tailnet.cpp

WINDOWS_NATIVE_LIBS := \
	-lws2_32 -liphlpapi -ld3d11 -ld3dcompiler -ldxgi -ldxguid \
	-lmfplat -lmf -lmfuuid -lwmcodecdspuuid -lmmdevapi -lavrt \
	-luser32 -lole32 -loleaut32 -luuid -lstrmiids -lksuser
WINDOWS_LIBS := -lcrypto $(AVLIBS) $(SWRLIBS) $(filter-out -mwindows,$(SDL3_LIBS)) -mconsole $(WINDOWS_NATIVE_LIBS)

deps-check:
	@set -e; \
	case "$(OS)" in Windows_NT) ;; *) \
		case "$$(uname -s 2>/dev/null || true)" in MINGW*|MSYS*|CYGWIN*) ;; \
		*) echo 'OPAL Windows build must run in MSYS2 MinGW/UCRT64.' >&2; exit 1;; esac;; \
	esac; \
	command -v "$(CXX)" >/dev/null 2>&1 || { echo 'Missing C++ compiler.' >&2; exit 1; }; \
	command -v "$(PKG_CONFIG)" >/dev/null 2>&1 || { echo 'Missing pkg-config.' >&2; exit 1; }; \
	missing=''; \
	for pkg in openssl sdl3 libavformat libavcodec libavutil libswresample; do \
		if ! "$(PKG_CONFIG)" --exists "$$pkg"; then missing="$$missing $$pkg"; fi; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Missing Windows build modules:$$missing" >&2; \
		echo 'MSYS2 UCRT64: install the toolchain, pkgconf, SDL3, OpenSSL and FFmpeg packages.' >&2; \
		exit 1; \
	fi

headless-deps-check:
	@set -e; \
	command -v powershell.exe >/dev/null 2>&1 || { echo 'PowerShell is required for Windows headless-display installation.' >&2; exit 1; }; \
	if [ -z "$(strip $(WINDOWS_MSBUILD))" ]; then \
		echo 'Windows headless display build requires Visual Studio MSBuild plus the Windows Driver Kit (WDK).' >&2; \
		exit 1; \
	fi

$(PRODUCT): $(WINDOWS_APP_SRCS) include/opal/*.hpp platform/windows/idd/Protocol.hpp src/platform/windows/cursor_compositor.hpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WINDOWS_APP_SRCS) $(LDFLAGS) $(WINDOWS_LIBS) -o $@

$(INPUT): src/platform/windows/input_helper.cpp include/opal/input_record.hpp include/opal/input_wire.hpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/platform/windows/input_helper.cpp -luser32 -o $@

$(WINDOWS_IDD_INSTALLER): platform/windows/idd/Install.cpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -municode platform/windows/idd/Install.cpp -lsetupapi -lnewdev -o $@

$(WINDOWS_IDD_STAMP): platform/windows/idd/Driver.cpp platform/windows/idd/Driver.hpp platform/windows/idd/Protocol.hpp platform/windows/idd/OpalDisplay.inf platform/windows/idd/OpalDisplay.vcxproj | $(BUILD) headless-deps-check
	@set -e; \
	mkdir -p "$(BUILD)/idd"; \
	"$(WINDOWS_MSBUILD)" "$(WINDOWS_IDD_PROJECT)" /nologo /m /t:Build /p:Configuration=Release /p:Platform=$(WINDOWS_IDD_PLATFORM); \
	dll="$$(find "$(BUILD)/idd" -type f -iname 'OPALDisplay.dll' -print -quit)"; \
	inf="$$(find "$(BUILD)/idd" -type f -iname 'OpalDisplay.inf' -print -quit)"; \
	[ -n "$$dll" ] && [ -n "$$inf" ] || { echo 'WDK build completed without a usable OPAL display-driver package.' >&2; exit 1; }; \
	mkdir -p "$(@D)"; \
	touch "$@"

windows-headless: $(WINDOWS_IDD_INSTALLER) $(WINDOWS_IDD_STAMP)
	@echo "OPAL Windows virtual display package built for $(WINDOWS_IDD_PLATFORM)."

all: $(PRODUCT) $(INPUT)

install: all
	@set -e; \
	command -v cygpath >/dev/null 2>&1 || { echo 'cygpath is required; run make install from the MSYS2 UCRT64 shell.' >&2; exit 1; }; \
	command -v powershell.exe >/dev/null 2>&1 || { echo 'PowerShell is required for Windows PATH registration.' >&2; exit 1; }; \
	$(INSTALL) -d "$(WINDOWS_BINDIR)"; \
	$(INSTALL) -m 0755 "$(PRODUCT)" "$(WINDOWS_BINDIR)/opal.exe"; \
	$(INSTALL) -m 0755 "$(INPUT)" "$(WINDOWS_BINDIR)/opal-input.exe"; \
	if [ -n "$(strip $(WINDOWS_MSBUILD))" ]; then \
		$(MAKE) --no-print-directory windows-headless; \
		inf="$$(find "$(BUILD)/idd" -type f -iname 'OpalDisplay.inf' -print -quit)"; \
		[ -n "$$inf" ] || { echo 'Could not locate built OPAL display-driver INF.' >&2; exit 1; }; \
		installer_win="$$(cygpath -w "$(WINDOWS_IDD_INSTALLER)")"; \
		inf_win="$$(cygpath -w "$$inf")"; \
		if OPAL_IDD_INSTALLER="$$installer_win" OPAL_IDD_INF="$$inf_win" powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '$$p=Start-Process -FilePath $$env:OPAL_IDD_INSTALLER -ArgumentList @("install") -Verb RunAs -Wait -PassThru; exit $$p.ExitCode'; then \
			:; \
		else \
			rc=$$?; \
			if [ "$$rc" -eq 10 ]; then echo 'OPAL virtual display driver installed; Windows restart required.'; \
			else echo "OPAL virtual display driver installation failed (exit $$rc). Check driver signing policy and WDK package output." >&2; exit "$$rc"; fi; \
		fi; \
		$(INSTALL) -m 0755 "$(WINDOWS_IDD_INSTALLER)" "$(WINDOWS_BINDIR)/opal-display-install.exe"; \
	else \
		echo 'OPAL headless Windows support not installed: Visual Studio MSBuild + WDK were not found. Monitor-attached OPAL remains available.'; \
	fi; \
	win_bin="$$(cygpath -w "$(WINDOWS_BINDIR)")"; \
	OPAL_INSTALL_BIN="$$win_bin" powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '$$bin = $$env:OPAL_INSTALL_BIN; $$path = [Environment]::GetEnvironmentVariable("Path", "User"); $$parts = @($$path -split ";" | Where-Object { -not [string]::IsNullOrWhiteSpace($$_) -and $$_ -ne $$bin -and $$_ -notmatch "(?i)\\opal\\build$$" }); $$newPath = (@($$bin) + $$parts) -join ";"; [Environment]::SetEnvironmentVariable("Path", $$newPath, "User"); Write-Host "Installed OPAL to $$bin"'; \
	"$(WINDOWS_BINDIR)/opal.exe" version

uninstall:
	@set -e; \
	if [ -f "$(WINDOWS_BINDIR)/opal-display-install.exe" ] && command -v cygpath >/dev/null 2>&1 && command -v powershell.exe >/dev/null 2>&1; then \
		installer_win="$$(cygpath -w "$(WINDOWS_BINDIR)/opal-display-install.exe")"; \
		OPAL_IDD_INSTALLER="$$installer_win" powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '$$p=Start-Process -FilePath $$env:OPAL_IDD_INSTALLER -ArgumentList @("uninstall") -Verb RunAs -Wait -PassThru; exit $$p.ExitCode' || true; \
	fi; \
	rm -f "$(WINDOWS_BINDIR)/opal.exe" "$(WINDOWS_BINDIR)/opal-input.exe" "$(WINDOWS_BINDIR)/opal-display-install.exe"; \
	echo "Removed OPAL from $(WINDOWS_BINDIR)."

else

$(error Unsupported platform '$(UNAME_S)'; OPAL supports Linux, Apple Silicon macOS, and Windows)

endif

ifneq ($(OPAL_OS),windows)

$(RENDEZVOUS_SERVER): src/rendezvous_main.cpp $(RENDEZVOUS_STATE_SRCS) $(RENDEZVOUS_PROTOCOL_SRCS) $(RELAY_SRCS) src/crypto.cpp include/opal/*.hpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/rendezvous_main.cpp $(RENDEZVOUS_STATE_SRCS) $(RENDEZVOUS_PROTOCOL_SRCS) $(RELAY_SRCS) src/crypto.cpp $(RENDEZVOUS_LIBS) -o $@

rendezvous-server: $(RENDEZVOUS_SERVER)

install-rendezvous: $(RENDEZVOUS_SERVER)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 "$(RENDEZVOUS_SERVER)" "$(DESTDIR)$(BINDIR)/opal-rendezvous"

endif

ifeq ($(OPAL_OS),linux)

deploy-rendezvous: rendezvous-server
	@if [ "$$(id -u)" -eq 0 ]; then echo 'Run as a normal user with sudo access, not as root.' >&2; exit 2; fi
	@set -e; \
	command -v sudo >/dev/null 2>&1 || { echo 'sudo is required' >&2; exit 1; }; \
	sudo install -m 0755 "$(RENDEZVOUS_SERVER)" /usr/local/bin/opal-rendezvous; \
	sudo install -m 0644 system/opal-rendezvous.service /etc/systemd/system/opal-rendezvous.service; \
	sudo mkdir -p /etc/systemd/system/opal-rendezvous.service.d; \
	printf '%s\n' '[Service]' 'Environment=OPAL_RENDEZVOUS_BIND=::' 'Environment=OPAL_RENDEZVOUS_PUBLIC_HOST=$(PUBLIC_HOST)' 'Environment=OPAL_RENDEZVOUS_PORT=$(RENDEZVOUS_PORT)' | sudo tee /etc/systemd/system/opal-rendezvous.service.d/endpoint.conf >/dev/null; \
	if command -v ufw >/dev/null 2>&1 && sudo ufw status 2>/dev/null | grep -q '^Status: active'; then sudo ufw allow '$(RENDEZVOUS_PORT)/udp'; fi; \
	if command -v firewall-cmd >/dev/null 2>&1 && sudo firewall-cmd --state >/dev/null 2>&1; then sudo firewall-cmd --permanent --add-port='$(RENDEZVOUS_PORT)/udp'; sudo firewall-cmd --reload; fi; \
	sudo systemctl daemon-reload; \
	sudo systemctl enable --now opal-rendezvous.service; \
	sudo systemctl --no-pager --full status opal-rendezvous.service

endif

verify: all
	@echo "OPAL $(OPAL_OS) build complete."

clean:
	rm -rf "$(BUILD)"

help:
	@echo 'OPAL build'
	@echo '  make              build OPAL'
	@echo '  make install      install/copy OPAL for this platform'
	@echo '  make uninstall    remove installed OPAL files'
	@echo '  make verify       build verification'
	@echo '  make clean        remove build output'
	@if [ "$(OPAL_OS)" != windows ]; then echo '  make rendezvous-server'; fi
	@if [ "$(OPAL_OS)" = windows ]; then echo '  make windows-headless  build the optional IddCx virtual-display package'; fi

.PHONY: all deps-check headless-deps-check install uninstall verify clean help rendezvous-server install-rendezvous deploy-rendezvous firewall-install firewall-remove macos-sign windows-headless
