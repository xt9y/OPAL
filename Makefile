include Makefile.core

CXXFLAGS += -pthread
FLV_STREAM_SRCS := src/flv_stream.cpp
VIDEO_CAPTURE_SRCS += $(FLV_STREAM_SRCS)
DIRECT_SENDER_SRCS += $(FLV_STREAM_SRCS)
APP_SRCS += $(FLV_STREAM_SRCS)
CLIPBOARD_SRCS := src/clipboard.cpp
APP_SRCS += $(CLIPBOARD_SRCS)
$(PRODUCT): $(CLIPBOARD_SRCS) $(FLV_STREAM_SRCS)
$(INPUT): include/opal/input_record.hpp
OPAL_SOAK_SECONDS ?= 3600
FFMPEG_H264_PROBE := (ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=32x32:rate=1 -frames:v 1 -pix_fmt yuv420p -c:v libx264 -bf 0 -g 1 -preset ultrafast -tune zerolatency -keyint_min 1 -sc_threshold 0 -an -f flv - >/dev/null 2>&1 || ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=32x32:rate=1 -frames:v 1 -pix_fmt yuv420p -c:v libopenh264 -bf 0 -g 1 -an -f flv - >/dev/null 2>&1)
REAL_FFMPEG := $(shell command -v ffmpeg 2>/dev/null)
INTEGRATION_FFMPEG_DIR := $(BUILD)/integration-bin
INTEGRATION_FFMPEG := $(INTEGRATION_FFMPEG_DIR)/ffmpeg
LINKED_CODEC_PROBE := $(BUILD)/test-linked-codec-probe

$(LINKED_CODEC_PROBE): tests/test_linked_codec_probe.cpp tests/linked_codec_support.hpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_linked_codec_probe.cpp $(AVLIBS) -o $@

$(INTEGRATION_FFMPEG): $(LINKED_CODEC_PROBE) | $(BUILD)
	mkdir -p "$(INTEGRATION_FFMPEG_DIR)"
	cat >"$@" <<'SH'
	#!/bin/sh
	real='$(REAL_FFMPEG)'
	decoder_probe='$(abspath $(LINKED_CODEC_PROBE))'
	if [ -z "$$real" ] || [ ! -x "$$real" ]; then exit 127; fi
	case " $$* " in
	  *' -encoders '*)
		if "$$decoder_probe" --check >/dev/null 2>&1 && "$$real" -hide_banner -loglevel error -f lavfi -i testsrc=size=32x32:rate=1 -frames:v 1 -pix_fmt yuv420p -c:v libx264 -bf 0 -g 1 -preset ultrafast -tune zerolatency -keyint_min 1 -sc_threshold 0 -an -f flv - >/dev/null 2>&1; then
			exec "$$real" "$$@"
		fi
		"$$real" "$$@" | sed '/[[:space:]]libx264[[:space:]]/d'
		;;
	  *) exec "$$real" "$$@" ;;
	esac
	SH
	chmod +x "$@"

test-linked-codec-probe: $(LINKED_CODEC_PROBE)
	$(LINKED_CODEC_PROBE)

test-flv-stream: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_flv_stream.cpp $(FLV_STREAM_SRCS) -o $(BUILD)/test-flv-stream
	$(BUILD)/test-flv-stream

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

test-capture-probe: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_capture_probe.cpp -o $(BUILD)/test-capture-probe
	$(BUILD)/test-capture-probe

test: test-flv-stream test-clipboard test-tailnet-discovery-lifecycle test-input-record test-latency-window test-capture-probe test-linked-codec-probe

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
test-direct-media-sanitize: test-flv-stream

test-thread-sanitize: CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -pthread $(SAN_COMMON) $(TSAN)
test-thread-sanitize: LDFLAGS += $(TSAN)
test-thread-sanitize: export TSAN_OPTIONS := halt_on_error=1:history_size=7
test-thread-sanitize: test-reliable-control test-peer-session test-peer-session-relay test-video-packet

NETEM_PIPELINE := $(BUILD)/test-direct-video-pipeline-netem
$(NETEM_PIPELINE): tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp -lcrypto $(AVLIBS) $(AUDIOLIBS) $(GLLIBS) -o $@

# Real kernel networking faults. Missing host tools are reported as an explicit
# skip; ordinary HPI correctness does not depend on root/network-namespace setup.
test-netem: $(NETEM_PIPELINE) $(LINKED_CODEC_PROBE)
	@if ! command -v tc >/dev/null 2>&1; then echo 'SKIP test-netem: install iproute2/tc (Fedora: sudo dnf install iproute-tc)'; exit 0; fi
	if ! command -v ip >/dev/null 2>&1; then echo 'SKIP test-netem: install iproute2/ip'; exit 0; fi
	if ! command -v timeout >/dev/null 2>&1; then echo 'SKIP test-netem: timeout command unavailable'; exit 0; fi
	if ! $(FFMPEG_H264_PROBE); then echo 'SKIP test-netem: FFmpeg has no executable libx264/libopenh264 encoder'; exit 0; fi
	if ! "$(LINKED_CODEC_PROBE)" --check; then echo 'SKIP test-netem: linked libavcodec has no H.264 decoder (Fedora: install RPM Fusion libavcodec-freeworld/ffmpeg-libs)'; exit 0; fi
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
# If encode/decode support is unavailable, deterministic non-capture stress tests still run.
test-soak: test-peer-session test-udp-transport test-direct-video-stress test-direct-video-pipeline $(LINKED_CODEC_PROBE)
	@case '$(OPAL_SOAK_SECONDS)' in ''|*[!0-9]*) echo 'OPAL_SOAK_SECONDS must be a positive integer' >&2; exit 2;; esac
	[ '$(OPAL_SOAK_SECONDS)' -gt 0 ] || { echo 'OPAL_SOAK_SECONDS must be > 0' >&2; exit 2; }
	start=$$(date +%s); deadline=$$((start + $(OPAL_SOAK_SECONDS))); iterations=0
	have_pipeline=0
	if $(FFMPEG_H264_PROBE) && "$(LINKED_CODEC_PROBE)" --check; then have_pipeline=1; else echo 'SKIP pipeline portion of soak: usable FFmpeg H.264 encode + linked H.264 decode are required'; fi
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
test-hpi: test-flv-stream test-capture-probe test-linked-codec-probe test-input test-media test-udp-transport test-video-packet test-video-reassembly test-video-feedback test-video-decoder test-video-present test-direct-video-stress test-direct-video-pipeline test-peer-session

# Always rebuild sanitizer binaries from scratch. ASan/UBSan and TSan are run
# in separate clean trees because they are not link-compatible with each other.
test-sanitize:
	$(MAKE) clean
	$(MAKE) -B test-direct-media-sanitize
	$(MAKE) clean
	$(MAKE) -B test-thread-sanitize

test-hpi-sanitize: test-sanitize

.PHONY: test-flv-stream test-input-record test-latency-window test-capture-probe test-linked-codec-probe test-thread-sanitize test-sanitize test-netem test-soak test-hpi test-hpi-sanitize

# The integration test validates networking/recovery in a headless process.
# Real presentation is covered separately by test-video-present and machine
# acceptance on Wayland/X11.
test-integration: $(INTEGRATION_FFMPEG) $(LINKED_CODEC_PROBE)
test-integration: export OPAL_TEST_HEADLESS=1
test-integration: export PATH := $(abspath $(INTEGRATION_FFMPEG_DIR)):$(PATH)
