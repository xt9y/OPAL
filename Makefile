CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
AVLIBS := -lavformat -lavcodec -lavutil
AUDIOLIBS := -lswresample -lpulse-simple -lpulse
GLLIBS := -lGL
NATIVE_MEDIA_LIBS := $(AVLIBS) $(AUDIOLIBS) $(GLLIBS)
LDLIBS += -lssl -lcrypto -lX11 -lXi -lpthread $(NATIVE_MEDIA_LIBS)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec/opal
SYSTEMDUSERDIR ?= $(PREFIX)/lib/systemd/user
UDEVDIR ?= /usr/lib/udev/rules.d
INSTALL ?= install
BUILD := build
PRODUCT := $(BUILD)/opal
INPUT := $(BUILD)/opal-input

PROFILE_SRCS := src/media_profile.cpp
VIDEO_CAPTURE_SRCS := src/video_capture.cpp
UDP_TRANSPORT_SRCS := src/udp_transport.cpp
VIDEO_CRYPTO_SRCS := src/video_crypto.cpp
VIDEO_PACKET_SRCS := src/video_packet.cpp
VIDEO_REASSEMBLY_SRCS := src/video_reassembly.cpp
DIRECT_VIDEO_SESSION_SRCS := src/direct_video_session.cpp
VIDEO_DECODER_SRCS := src/video_decoder.cpp
VIDEO_PRESENT_SRCS := src/video_present.cpp
VIDEO_FEEDBACK_SRCS := src/video_feedback.cpp
AUDIO_OUTPUT_SRCS := src/audio_output.cpp
VIDEO_SENDER_SRCS := src/video_sender.cpp
VIDEO_RECEIVER_SRCS := src/video_receiver.cpp
CORE_SRCS := src/config.cpp src/crypto.cpp src/media.cpp src/wake.cpp
NET_SRCS := src/net.cpp src/config.cpp src/crypto.cpp
INPUT_SRCS := src/input.cpp
TUNNEL_SUPERVISOR_SRCS := src/tunnel_supervisor.cpp
DIRECT_VIDEO_BASE_SRCS := $(DIRECT_VIDEO_SESSION_SRCS) $(UDP_TRANSPORT_SRCS) $(VIDEO_CRYPTO_SRCS) $(VIDEO_PACKET_SRCS)
DIRECT_MEDIA_COMMON_SRCS := $(VIDEO_FEEDBACK_SRCS)
DIRECT_RECEIVER_SRCS := $(VIDEO_RECEIVER_SRCS) $(VIDEO_REASSEMBLY_SRCS) $(VIDEO_DECODER_SRCS) $(VIDEO_PRESENT_SRCS) $(AUDIO_OUTPUT_SRCS)
DIRECT_SENDER_SRCS := $(VIDEO_SENDER_SRCS) $(VIDEO_CAPTURE_SRCS)
DIRECT_MEDIA_SRCS := $(DIRECT_VIDEO_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS)
SESSION_SRCS := src/session.cpp src/net.cpp src/tunnel_access.cpp $(DIRECT_VIDEO_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) src/config.cpp src/crypto.cpp
APP_SRCS := src/main.cpp src/setup.cpp src/host.cpp src/client.cpp src/session.cpp src/net.cpp src/tunnel.cpp src/tunnel_access.cpp src/zrok_cleanup.cpp $(TUNNEL_SUPERVISOR_SRCS) src/system.cpp $(CORE_SRCS) $(PROFILE_SRCS) $(DIRECT_MEDIA_SRCS) $(INPUT_SRCS)

all: $(PRODUCT) $(INPUT)

$(BUILD):
	mkdir -p $(BUILD)

$(PRODUCT): $(APP_SRCS) include/opal/*.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(APP_SRCS) $(LDFLAGS) $(LDLIBS) -o $@

$(INPUT): src/input_helper.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/input_helper.cpp -o $@

test-core: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_core.cpp $(CORE_SRCS) -lcrypto -o $(BUILD)/test-core
	$(BUILD)/test-core

test-media-profile: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_media_profile.cpp $(PROFILE_SRCS) -o $(BUILD)/test-media-profile
	$(BUILD)/test-media-profile

test-media: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_media.cpp src/media.cpp $(PROFILE_SRCS) -o $(BUILD)/test-media
	$(BUILD)/test-media

test-video-capture: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_capture.cpp $(VIDEO_CAPTURE_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp $(AVLIBS) -o $(BUILD)/test-video-capture
	$(BUILD)/test-video-capture

test-udp-transport: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_udp_transport.cpp $(UDP_TRANSPORT_SRCS) -lpthread -o $(BUILD)/test-udp-transport
	$(BUILD)/test-udp-transport

test-video-crypto: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_crypto.cpp $(VIDEO_CRYPTO_SRCS) $(NET_SRCS) -lssl -lcrypto -lpthread -o $(BUILD)/test-video-crypto
	$(BUILD)/test-video-crypto

test-video-packet: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_packet.cpp $(VIDEO_PACKET_SRCS) -o $(BUILD)/test-video-packet
	$(BUILD)/test-video-packet

test-video-reassembly: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_reassembly.cpp $(VIDEO_PACKET_SRCS) $(VIDEO_REASSEMBLY_SRCS) -o $(BUILD)/test-video-reassembly
	$(BUILD)/test-video-reassembly

test-direct-video-session: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_session.cpp $(DIRECT_VIDEO_BASE_SRCS) $(NET_SRCS) -lssl -lcrypto -lpthread -o $(BUILD)/test-direct-video-session
	$(BUILD)/test-direct-video-session

test-direct-video-security: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_security.cpp $(DIRECT_VIDEO_BASE_SRCS) $(NET_SRCS) -lssl -lcrypto -lpthread -o $(BUILD)/test-direct-video-security
	$(BUILD)/test-direct-video-security

test-video-decoder: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_decoder.cpp $(VIDEO_DECODER_SRCS) $(VIDEO_CAPTURE_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp $(AVLIBS) -o $(BUILD)/test-video-decoder
	$(BUILD)/test-video-decoder

test-video-present: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_present.cpp $(VIDEO_PRESENT_SRCS) -lavutil -lX11 $(GLLIBS) -o $(BUILD)/test-video-present
	$(BUILD)/test-video-present

test-audio-output: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_audio_output.cpp $(AUDIO_OUTPUT_SRCS) -lavcodec -lavutil $(AUDIOLIBS) -lpthread -o $(BUILD)/test-audio-output
	$(BUILD)/test-audio-output

test-video-feedback: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_feedback.cpp $(VIDEO_FEEDBACK_SRCS) -o $(BUILD)/test-video-feedback
	$(BUILD)/test-video-feedback

test-direct-video-pipeline: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_direct_video_pipeline.cpp $(DIRECT_VIDEO_BASE_SRCS) $(DIRECT_MEDIA_COMMON_SRCS) $(DIRECT_RECEIVER_SRCS) $(DIRECT_SENDER_SRCS) src/media.cpp $(PROFILE_SRCS) src/config.cpp -lssl -lcrypto -lX11 -lpthread $(NATIVE_MEDIA_LIBS) -o $(BUILD)/test-direct-video-pipeline
	$(BUILD)/test-direct-video-pipeline

test-input: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_input.cpp $(INPUT_SRCS) -o $(BUILD)/test-input
	$(BUILD)/test-input

test-setup: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_setup.cpp src/setup.cpp src/config.cpp $(PROFILE_SRCS) -o $(BUILD)/test-setup
	$(BUILD)/test-setup

test-daemon: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_daemon.cpp -o $(BUILD)/test-daemon
	$(BUILD)/test-daemon

test-tunnel: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_tunnel.cpp src/tunnel.cpp src/config.cpp src/crypto.cpp -lssl -lcrypto -o $(BUILD)/test-tunnel
	$(BUILD)/test-tunnel

test-tunnel-recovery: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_tunnel_recovery.cpp src/tunnel.cpp $(TUNNEL_SUPERVISOR_SRCS) src/config.cpp src/crypto.cpp -lssl -lcrypto -lpthread -o $(BUILD)/test-tunnel-recovery
	$(BUILD)/test-tunnel-recovery

test-net: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_net.cpp $(NET_SRCS) -lssl -lcrypto -lpthread -o $(BUILD)/test-net
	$(BUILD)/test-net

test-session: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_session.cpp $(SESSION_SRCS) -lssl -lcrypto -lX11 -lpthread $(NATIVE_MEDIA_LIBS) -o $(BUILD)/test-session
	$(BUILD)/test-session

test-hardening: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_hardening.cpp src/crypto.cpp -lcrypto -o $(BUILD)/test-hardening
	$(BUILD)/test-hardening

test-direct-media-sanitize: test-video-crypto test-video-packet test-video-reassembly test-direct-video-session test-direct-video-security test-video-capture test-video-decoder test-video-present test-audio-output test-video-feedback test-direct-video-pipeline

test-clean: all
	BIN=$(abspath $(PRODUCT)) sh ./tests/test_clean.sh

test-install: all
	MAKE=$(MAKE) sh ./tests/test_install.sh

test: all test-core test-media-profile test-media test-video-capture test-udp-transport test-video-crypto test-video-packet test-video-reassembly test-direct-video-session test-direct-video-security test-video-decoder test-video-present test-audio-output test-video-feedback test-direct-video-pipeline test-input test-setup test-daemon test-tunnel test-tunnel-recovery test-net test-session test-hardening test-clean test-install
	BIN=$(abspath $(PRODUCT)) INPUT_BIN=$(abspath $(INPUT)) ./tests/smoke.sh
	BIN=$(abspath $(PRODUCT)) INPUT_BIN=$(abspath $(INPUT)) ./tests/integration.sh
	@tmp=$$(mktemp -d); \
	  $(MAKE) install DESTDIR=$$tmp >/dev/null; \
	  test -x "$$tmp$(BINDIR)/opal"; \
	  test -x "$$tmp$(LIBEXECDIR)/opal-input"; \
	  test -f "$$tmp$(SYSTEMDUSERDIR)/opal-host.service"; \
	  test -f "$$tmp$(UDEVDIR)/70-opal-uinput.rules"; \
	  "$$tmp$(BINDIR)/opal" version | grep -q '^OPAL 0.1.0$$'; \
	  $(MAKE) uninstall DESTDIR=$$tmp >/dev/null; \
	  test ! -e "$$tmp$(BINDIR)/opal"; \
	  rm -rf $$tmp; \
	  echo 'install tests passed'

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBEXECDIR)" "$(DESTDIR)$(SYSTEMDUSERDIR)" "$(DESTDIR)$(UDEVDIR)"
	$(INSTALL) -m 0755 $(PRODUCT) "$(DESTDIR)$(BINDIR)/opal"
	$(INSTALL) -m 0755 $(INPUT) "$(DESTDIR)$(LIBEXECDIR)/opal-input"
	$(INSTALL) -m 0644 systemd/opal-host.service "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-host.service"
	$(INSTALL) -m 0644 systemd/opal-bridge.service "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-bridge.service"
	$(INSTALL) -m 0644 packaging/70-opal-uinput.rules "$(DESTDIR)$(UDEVDIR)/70-opal-uinput.rules"
	@if [ -z "$(DESTDIR)" ]; then \
	  if command -v modprobe >/dev/null 2>&1; then modprobe uinput || true; fi; \
	  if command -v udevadm >/dev/null 2>&1; then \
	    udevadm control --reload-rules; \
	    udevadm trigger --action=change --sysname-match=uinput; \
	    udevadm settle; \
	  fi; \
	fi
	@echo "Installed OPAL to $(DESTDIR)$(PREFIX)"
	@echo "Run: opal"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/opal" "$(DESTDIR)$(LIBEXECDIR)/opal-input"
	rm -f "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-host.service" "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-bridge.service"
	-rmdir "$(DESTDIR)$(LIBEXECDIR)" 2>/dev/null

clean:
	rm -rf $(BUILD)

.PHONY: all test test-core test-media-profile test-media test-video-capture test-udp-transport test-video-crypto test-video-packet test-video-reassembly test-direct-video-session test-direct-video-security test-video-decoder test-video-present test-audio-output test-video-feedback test-direct-video-pipeline test-direct-media-sanitize test-input test-setup test-daemon test-tunnel test-tunnel-recovery test-net test-session test-hardening test-clean test-install install uninstall clean
