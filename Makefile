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
CAPTURE_PROBE := $(BUILD)/test-capture-probe
FFMPEG_H264_PROBE := "$(CAPTURE_PROBE)" --check
REAL_FFMPEG := $(shell command -v ffmpeg 2>/dev/null)
INTEGRATION_FFMPEG_DIR := $(BUILD)/integration-bin
INTEGRATION_FFMPEG := $(INTEGRATION_FFMPEG_DIR)/ffmpeg
LINKED_CODEC_PROBE := $(BUILD)/test-linked-codec-probe

$(LINKED_CODEC_PROBE): tests/test_linked_codec_probe.cpp tests/linked_codec_support.hpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_linked_codec_probe.cpp $(AVLIBS) -o $@

$(CAPTURE_PROBE): tests/test_capture_probe.cpp tests/capture_test_support.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_capture_probe.cpp -o $@

$(INTEGRATION_FFMPEG): $(LINKED_CODEC_PROBE) | $(BUILD)
	mkdir -p "$(INTEGRATION_FFMPEG_DIR)"
	cat >"$@" <<'SH'
	#!/bin/sh
	real='$(REAL_FFMPEG)'
	decoder_probe='$(abspath $(LINKED_CODEC_PROBE))'
	if [ -z "$$real" ] || [ ! -x "$$real" ]; then exit 127; fi
	case " $$* " in
	  *' -encoders '*)
		tmp=$$(mktemp)
		trap 'rm -f "$$tmp"' EXIT INT TERM
		if "$$decoder_probe" --check >/dev/null 2>&1 && "$$real" -nostdin -hide_banner -loglevel error -f lavfi -i testsrc=size=320x180:rate=60 -frames:v 8 -pix_fmt yuv420p -c:v libx264 -bf 0 -g 4 -preset ultrafast -tune zerolatency -keyint_min 4 -sc_threshold 0 -an -flush_packets 1 -f flv pipe:1 >"$$tmp" 2>/dev/null && [ -s "$$tmp" ]; then
			rm -f "$$tmp"; trap - EXIT INT TERM; exec "$$real" "$$@"
		fi
		rm -f "$$tmp"; trap - EXIT INT TERM
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

test-video-reorder: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_reorder.cpp $(VIDEO_PACKET_SRCS) $(VIDEO_REASSEMBLY_SRCS) -o $(BUILD)/test-video-reorder
	$(BUILD)/test-video-reorder

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

test-capture-probe: $(CAPTURE_PROBE)
	$(CAPTURE_PROBE)

test: test-flv-stream test-video-reorder test-clipboard test-tailnet-discovery-lifecycle test-input-record test-latency-window test-capture-probe test-linked-codec-probe

SAN_COMMON := -O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls
ASAN_UBSAN := -fsanitize=address,undefined
TSAN := -fsanitize=thread

test-direct-media-sanitize: CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -pthread $(SAN_COMMON) $(ASAN_UBSAN)
test-direct-media-sanitize: LDFLAGS += $(ASAN_UBSAN)
test-direct-media-sanitize: export ASAN_OPTIONS := detect_leaks=1:strict_string_checks=1:check_initialization_order=1
test-direct-media-sanitize: export UBSAN_OPTIONS := print_stacktrace=1:halt_on_error=1
test-direct-media-sanitize: test-flv-stream test-video-reorder

test-thread-sanitize-run: CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -pthread $(SAN_COMMON) $(TSAN)
test-thread-sanitize-run: LDFLAGS += $(TSAN)
test-thread-sanitize-run: export TSAN_OPTIONS := halt_on_error=1:history_size=7
test-thread-sanitize-run: test-reliable-control test-video-crypto test-peer-handshake test-session-packet test-relay test-udp-transport test-video-packet

test-thread-sanitize-session-run: CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -pthread $(SAN_COMMON) $(TSAN)
test-thread-sanitize-session-run: LDFLAGS += $(TSAN)
test-thread-sanitize-session-run: export TSAN_OPTIONS := halt_on_error=1:history_size=7
test-thread-sanitize-session-run: export OPAL_TEST_TRACE := 1
test-thread-sanitize-session-run: test-peer-session test-peer-session-relay

test-thread-sanitize:
	mkdir -p "$(BUILD)"
	tmp="$(BUILD)/.tsan-runtime-probe"
	cat >"$$tmp.cpp" <<'CPP'
	#include <openssl/evp.h>
	#include <thread>
	static bool keygen(int id){EVP_PKEY_CTX*ctx=EVP_PKEY_CTX_new_id(id,nullptr);if(!ctx)return false;EVP_PKEY*key=nullptr;const bool ok=EVP_PKEY_keygen_init(ctx)==1&&EVP_PKEY_keygen(ctx,&key)==1;EVP_PKEY_free(key);EVP_PKEY_CTX_free(ctx);return ok;}
	int main(){bool ok=false;std::thread t([&]{ok=keygen(EVP_PKEY_ED25519)&&keygen(EVP_PKEY_X25519);});t.join();return ok?0:1;}
	CPP
	if ! $(CXX) -std=c++20 -pthread "$$tmp.cpp" $(TSAN) -lcrypto -o "$$tmp" >/dev/null 2>&1; then
		echo 'SKIP test-thread-sanitize: compiler cannot link ThreadSanitizer + OpenSSL runtime on this system'
		rm -f "$$tmp" "$$tmp.cpp"
		exit 0
	fi
	if ! TSAN_OPTIONS=halt_on_error=1 "$$tmp" >/dev/null 2>&1; then
		echo 'SKIP test-thread-sanitize: ThreadSanitizer + OpenSSL runtime cannot execute on this system'
		rm -f "$$tmp" "$$tmp.cpp"
		exit 0
	fi
	rm -f "$$tmp" "$$tmp.cpp"
	$(MAKE) -B test-thread-sanitize-run
	tmp="$(BUILD)/.tsan-condvar-probe"
	cat >"$$tmp.cpp" <<'CPP'
	#include <chrono>
	#include <condition_variable>
	#include <mutex>
	#include <thread>
	int main(){std::mutex m;std::condition_variable cv;bool ready=false;std::thread t([&]{std::unique_lock<std::mutex>lock(m);cv.wait_for(lock,std::chrono::milliseconds(20),[&]{return ready;});});{std::lock_guard<std::mutex>lock(m);ready=true;}cv.notify_one();t.join();return 0;}
	CPP
	if ! $(CXX) -std=c++20 -pthread "$$tmp.cpp" $(TSAN) -o "$$tmp" >/dev/null 2>&1; then
		echo 'SKIP peer-session TSan: compiler cannot link condition-variable probe'
		rm -f "$$tmp" "$$tmp.cpp"
		exit 0
	fi
	if ! TSAN_OPTIONS=halt_on_error=1 "$$tmp" >/dev/null 2>&1; then
		echo 'SKIP peer-session TSan: ThreadSanitizer condition-variable interceptor cannot execute on this system'
		rm -f "$$tmp" "$$tmp.cpp"
		exit 0
	fi
	rm -f "$$tmp" "$$tmp.cpp"
	$(MAKE) -B test-thread-sanitize-session-run

NETEM_PIPELINE := $(BUILD)/test-direct-video-pipeline-netem
$(NETEM_PIPELINE): tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp | $(BUILD) deps-check
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_pipeline.cpp $(DIRECT_MEDIA_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp -lcrypto $(AVLIBS) $(AUDIOLIBS) $(GLLIBS) -o $@

test-netem: $(NETEM_PIPELINE) $(LINKED_CODEC_PROBE) $(CAPTURE_PROBE)
	@if ! command -v tc >/dev/null 2>&1; then echo 'SKIP test-netem: install iproute2/tc (Fedora: sudo dnf install iproute-tc)'; exit 0; fi
	if ! command -v ip >/dev/null 2>&1; then echo 'SKIP test-netem: install iproute2/ip'; exit 0; fi
	if ! command -v timeout >/dev/null 2>&1; then echo 'SKIP test-netem: timeout command unavailable'; exit 0; fi
	if ! $(FFMPEG_H264_PROBE) >/dev/null; then echo 'SKIP test-netem: FFmpeg has no H.264 encoder that emits a valid streaming FLV'; exit 0; fi
	if ! "$(LINKED_CODEC_PROBE)" --check; then echo 'SKIP test-netem: linked libavcodec has no H.264 decoder (Fedora: install RPM Fusion libavcodec-freeworld/ffmpeg-libs)'; exit 0; fi
	export BIN='$(abspath $(NETEM_PIPELINE))'
	run_cases='set -eu; \
		ip link set lo up; \
		cleanup(){ tc qdisc del dev lo root >/dev/null 2>&1 || true; }; \
		trap cleanup EXIT INT TERM; \
		run(){ name=$$1; shift; case "$$name" in clean|lan-jitter) loss=0;; *) loss=1;; esac; echo "netem $$name: $$*"; tc qdisc replace dev lo root netem "$$@"; OPAL_TEST_HEADLESS=1 OPAL_TEST_NETEM=1 OPAL_TEST_NETEM_LOSS=$$loss timeout 45 "$$BIN"; }; \
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

test-soak: test-peer-session test-udp-transport test-direct-video-stress test-direct-video-pipeline $(LINKED_CODEC_PROBE) $(CAPTURE_PROBE)
	@case '$(OPAL_SOAK_SECONDS)' in ''|*[!0-9]*) echo 'OPAL_SOAK_SECONDS must be a positive integer' >&2; exit 2;; esac
	[ '$(OPAL_SOAK_SECONDS)' -gt 0 ] || { echo 'OPAL_SOAK_SECONDS must be > 0' >&2; exit 2; }
	start=$$(date +%s); deadline=$$((start + $(OPAL_SOAK_SECONDS))); iterations=0
	have_pipeline=0
	if $(FFMPEG_H264_PROBE) >/dev/null && "$(LINKED_CODEC_PROBE)" --check; then have_pipeline=1; else echo 'SKIP pipeline portion of soak: usable streaming FFmpeg H.264 encode + linked H.264 decode are required'; fi
	while [ $$(date +%s) -lt $$deadline ]; do
		$(BUILD)/test-peer-session
		$(BUILD)/test-udp-transport
		$(BUILD)/test-direct-video-stress
		if [ $$have_pipeline -eq 1 ]; then OPAL_TEST_HEADLESS=1 $(BUILD)/test-direct-video-pipeline; fi
		iterations=$$((iterations + 1))
	done
	echo "HPI soak iterations=$$iterations seconds=$$(($$(date +%s) - start))"

test-hpi: test-flv-stream test-video-reorder test-capture-probe test-linked-codec-probe test-input test-media test-udp-transport test-video-packet test-video-reassembly test-video-feedback test-video-decoder test-video-present test-direct-video-stress test-direct-video-pipeline test-peer-session

test-sanitize:
	$(MAKE) clean
	$(MAKE) -B test-direct-media-sanitize
	$(MAKE) clean
	$(MAKE) -B test-thread-sanitize

test-hpi-sanitize: test-sanitize

.PHONY: test-flv-stream test-video-reorder test-input-record test-latency-window test-capture-probe test-linked-codec-probe test-thread-sanitize test-thread-sanitize-run test-thread-sanitize-session-run test-sanitize test-netem test-soak test-hpi test-hpi-sanitize

test-integration: $(INTEGRATION_FFMPEG) $(LINKED_CODEC_PROBE)
test-integration: export OPAL_TEST_HEADLESS=1
test-integration: export PATH := $(abspath $(INTEGRATION_FFMPEG_DIR)):$(PATH)
