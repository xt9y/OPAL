include Makefile.core

SDL3_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL3_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
CPPFLAGS += $(SDL3_CFLAGS)
CXXFLAGS += -pthread
LDLIBS := $(filter-out -lX11 -lXi,$(LDLIBS)) $(SDL3_LIBS)
CLIPBOARD_SRCS := src/clipboard.cpp
APP_SRCS += $(CLIPBOARD_SRCS)
$(PRODUCT): $(CLIPBOARD_SRCS)
$(INPUT): include/opal/input_record.hpp
OPAL_SOAK_SECONDS ?= 3600

# VideoReceiver no longer owns the window/presenter. Product APP_SRCS was
# assembled by Makefile.core before this override and still contains
# src/video_present.cpp; receiver-only tests do not need SDL/OpenGL windowing.
DIRECT_RECEIVER_SRCS := $(VIDEO_RECEIVER_SRCS) $(VIDEO_REASSEMBLY_SRCS) $(VIDEO_DECODER_SRCS) $(AUDIO_OUTPUT_SRCS)

deps-check:
	@PKG_CONFIG_BIN='$(PKG_CONFIG)'
	REQUIRED_PKGS='openssl sdl3 gl libpulse-simple libavformat libavcodec libavutil libswresample'
	if ! command -v "$$PKG_CONFIG_BIN" >/dev/null 2>&1; then
		echo 'OPAL build dependency check failed: pkg-config is not installed.' >&2
		missing=pkg-config
	else
		missing=''
		for pkg in $$REQUIRED_PKGS; do
			if ! "$$PKG_CONFIG_BIN" --exists "$$pkg"; then missing="$$missing $$pkg"; fi
		done
	fi
	if [ -n "$$missing" ]; then
		[ "$$missing" = pkg-config ] || { echo 'OPAL build dependency check failed.' >&2; echo "Missing pkg-config modules:$$missing" >&2; }
		id=''; like=''
		if [ -r /etc/os-release ]; then . /etc/os-release; id=$${ID:-}; like=$${ID_LIKE:-}; fi
		case "$$id $$like" in
			*fedora*|*rhel*) echo 'Install: sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL3-devel openssl-devel libglvnd-devel pulseaudio-libs-devel ffmpeg-free ffmpeg-free-devel' >&2 ;;
			*debian*|*ubuntu*) echo 'Install: sudo apt-get install -y g++ make pkg-config libsdl3-dev libssl-dev libgl1-mesa-dev libpulse-dev ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswresample-dev' >&2 ;;
			*arch*) echo 'Install: sudo pacman -S --needed base-devel pkgconf sdl3 openssl libglvnd libpulse ffmpeg' >&2 ;;
			*) echo 'Install a C++20 compiler, pkg-config, SDL3, OpenSSL, OpenGL, PulseAudio client libraries, and FFmpeg development files.' >&2 ;;
		esac
		exit 1
	fi

test-clipboard: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_clipboard.cpp $(CLIPBOARD_SRCS) -o $(BUILD)/test-clipboard
	$(BUILD)/test-clipboard

test-tailnet-discovery-lifecycle: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_tailnet_discovery_lifecycle.cpp -o $(BUILD)/test-tailnet-discovery-lifecycle
	$(BUILD)/test-tailnet-discovery-lifecycle

test-input-record: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_input_record.cpp -o $(BUILD)/test-input-record
	$(BUILD)/test-input-record

test-latency-window: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_latency_window.cpp -o $(BUILD)/test-latency-window
	$(BUILD)/test-latency-window

test: test-clipboard test-tailnet-discovery-lifecycle test-input-record test-latency-window

test-video-present: | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_present.cpp $(VIDEO_PRESENT_SRCS) -lavutil $(GLLIBS) $(SDL3_LIBS) -o $(BUILD)/test-video-present
	$(BUILD)/test-video-present

test-direct-video-pipeline: | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp -lcrypto -lpthread $(AVLIBS) $(AUDIOLIBS) $(GLLIBS) -o $(BUILD)/test-direct-video-pipeline
	@if ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])libx264([[:space:]]|$$)'; then \
		$(BUILD)/test-direct-video-pipeline; \
	else \
		echo 'SKIP test-direct-video-pipeline: FFmpeg libx264 encoder unavailable'; \
	fi

# Sanitizer targets propagate their flags to their prerequisites. The wrapper
# below forces clean rebuilds because make does not consider CXXFLAGS when it
# decides whether an existing test binary is up to date.
SAN_COMMON := -O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls
ASAN_UBSAN := -fsanitize=address,undefined
TSAN := -fsanitize=thread

test-direct-media-sanitize: CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -pthread $(SAN_COMMON) $(ASAN_UBSAN)
test-direct-media-sanitize: LDFLAGS += $(ASAN_UBSAN)
test-direct-media-sanitize: export ASAN_OPTIONS := detect_leaks=1:strict_string_checks=1:check_initialization_order=1
test-direct-media-sanitize: export UBSAN_OPTIONS := print_stacktrace=1:halt_on_error=1

test-thread-sanitize: CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -pthread $(SAN_COMMON) $(TSAN)
test-thread-sanitize: LDFLAGS += $(TSAN)
test-thread-sanitize: export TSAN_OPTIONS := halt_on_error=1:history_size=7
test-thread-sanitize: test-reliable-control test-peer-session test-peer-session-relay test-video-packet

NETEM_PIPELINE := $(BUILD)/test-direct-video-pipeline-netem
$(NETEM_PIPELINE): tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp -lcrypto $(AVLIBS) $(AUDIOLIBS) $(GLLIBS) -o $@

# Real kernel networking faults. Missing host tools are reported as an explicit
# skip; ordinary HPI correctness does not depend on root/network-namespace setup.
test-netem: $(NETEM_PIPELINE)
	@if ! command -v tc >/dev/null 2>&1; then echo 'SKIP test-netem: install iproute2/tc (Fedora: sudo dnf install iproute-tc)'; exit 0; fi
	@if ! command -v ip >/dev/null 2>&1; then echo 'SKIP test-netem: install iproute2/ip'; exit 0; fi
	@if ! command -v timeout >/dev/null 2>&1; then echo 'SKIP test-netem: timeout command unavailable'; exit 0; fi
	@if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])libx264([[:space:]]|$$)'; then echo 'SKIP test-netem: FFmpeg libx264 encoder unavailable'; exit 0; fi
	export BIN='$(abspath $(NETEM_PIPELINE))'
	run_cases='set -eu; \
		ip link set lo up; \
		cleanup(){ tc qdisc del dev lo root >/dev/null 2>&1 || true; }; \
		trap cleanup EXIT INT TERM; \
		run(){ name=$$1; shift; echo "netem $$name: $$*"; tc qdisc replace dev lo root netem "$$@"; OPAL_TEST_HEADLESS=1 timeout 45 "$$BIN"; }; \
		run clean delay 0ms; \
		run lan-jitter delay 3ms 1ms distribution normal; \
		run mild-loss delay 5ms 2ms loss 1%; \
		run bad-wifi delay 12ms 5ms loss 3% reorder 5% 50%; \
		run collapse delay 20ms 8ms loss 5% rate 2mbit; \
		cleanup'
	if command -v unshare >/dev/null 2>&1 && unshare -Urn sh -c 'ip link set lo up' >/dev/null 2>&1; then
		unshare -Urn sh -ec "$$run_cases"
	elif [ "$$(id -u)" -eq 0 ]; then
		sh -ec "$$run_cases"
	else
		echo 'SKIP test-netem: needs unprivileged user namespaces or sudo make test-netem'
	fi

# Repeated real threads, crypto, packetization, reassembly, decode and recovery.
# If libx264 is unavailable, the deterministic non-capture stress tests still run.
test-soak: test-peer-session test-udp-transport test-direct-video-stress test-direct-video-pipeline
	@case '$(OPAL_SOAK_SECONDS)' in ''|*[!0-9]*) echo 'OPAL_SOAK_SECONDS must be a positive integer' >&2; exit 2;; esac
	[ '$(OPAL_SOAK_SECONDS)' -gt 0 ] || { echo 'OPAL_SOAK_SECONDS must be > 0' >&2; exit 2; }
	start=$$(date +%s); deadline=$$((start + $(OPAL_SOAK_SECONDS))); iterations=0
	have_pipeline=0
	if ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^|[[:space:]])libx264([[:space:]]|$$)'; then have_pipeline=1; else echo 'SKIP pipeline portion of soak: FFmpeg libx264 encoder unavailable'; fi
	while [ $$(date +%s) -lt $$deadline ]; do
		$(BUILD)/test-peer-session
		$(BUILD)/test-udp-transport
		$(BUILD)/test-direct-video-stress
		if [ $$have_pipeline -eq 1 ]; then OPAL_TEST_HEADLESS=1 $(BUILD)/test-direct-video-pipeline; fi
		iterations=$$((iterations + 1))
	done
	echo "HPI soak iterations=$$iterations seconds=$$(($$(date +%s) - start))"

# Fast HPI correctness gate. Hostile kernel networking and sanitizers stay
# explicit because they are intentionally slower and environment-sensitive.
test-hpi: test-input test-media test-udp-transport test-video-packet test-video-reassembly test-video-feedback test-video-decoder test-video-present test-direct-video-stress test-direct-video-pipeline test-peer-session

# Always rebuild sanitizer binaries from scratch. ASan/UBSan and TSan are run
# in separate clean trees because they are not link-compatible with each other.
test-sanitize:
	$(MAKE) clean
	$(MAKE) -B test-direct-media-sanitize
	$(MAKE) clean
	$(MAKE) -B test-thread-sanitize

test-hpi-sanitize: test-sanitize

.PHONY: test-input-record test-latency-window test-thread-sanitize test-sanitize test-netem test-soak test-hpi test-hpi-sanitize

# The integration test validates networking/recovery in a headless process.
# Real presentation is covered separately by test-video-present and machine
# acceptance on Wayland/X11.
test-integration: export OPAL_TEST_HEADLESS=1
