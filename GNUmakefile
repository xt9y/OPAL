UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
OPAL_OS ?= $(if $(filter Windows_NT,$(OS)),windows,$(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),windows,$(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))))

# Apple's /usr/bin/make is GNU Make 3.81. Keep it as the public entrypoint, but
# transparently hand the requested goals to Homebrew GNU Make before any file
# using .ONESHELL is included. Once gmake re-enters this file, its `oneshell`
# feature is present and the normal macOS build below is selected.
ifeq ($(UNAME_S),Darwin)
ifeq ($(filter oneshell,$(.FEATURES)),)
opal_macos_forward_goal = $(if $(filter install,$1),macos-install,$(if $(filter all,$1),macos-all,$(if $(filter test,$1),macos-verify,$1)))
OPAL_GMAKE_GOALS := $(foreach goal,$(MAKECMDGOALS),$(call opal_macos_forward_goal,$(goal)))

.PHONY: __opal_gmake_forward $(MAKECMDGOALS)
__opal_gmake_forward:
	@command -v gmake >/dev/null 2>&1 || { echo "OPAL macOS requires Homebrew GNU Make. Install it once with: brew install make" >&2; exit 1; }
	@exec gmake $(OPAL_GMAKE_GOALS)

ifneq ($(strip $(MAKECMDGOALS)),)
$(MAKECMDGOALS): __opal_gmake_forward ;
else
.DEFAULT_GOAL := __opal_gmake_forward
endif
else
OPAL_NATIVE_MAKE := 1
endif
else
OPAL_NATIVE_MAKE := 1
endif

# Makefile.core contains Linux-era public targets named all/test/install.
# On Windows, forward those three front-door goals to explicit Windows targets
# before Makefile.core is parsed, so they cannot accidentally build/install the
# Linux service/input layout. Direct windows-* goals skip this wrapper.
ifeq ($(OPAL_OS),windows)
WINDOWS_PUBLIC_GOALS := $(filter all test install,$(MAKECMDGOALS))
ifneq ($(strip $(WINDOWS_PUBLIC_GOALS)),)
OPAL_NATIVE_MAKE := 0
opal_windows_forward_goal = $(if $(filter install,$1),windows-install,$(if $(filter test,$1),windows-test,windows-all))
OPAL_WINDOWS_GOALS := $(foreach goal,$(WINDOWS_PUBLIC_GOALS),$(call opal_windows_forward_goal,$(goal)))
.PHONY: __opal_windows_forward $(WINDOWS_PUBLIC_GOALS)
__opal_windows_forward:
	@$(MAKE) OPAL_OS=windows $(OPAL_WINDOWS_GOALS)
$(WINDOWS_PUBLIC_GOALS): __opal_windows_forward ;
endif
endif

ifeq ($(OPAL_NATIVE_MAKE),1)
ifeq ($(OPAL_OS),linux)
include Makefile
else ifeq ($(OPAL_OS),macos)
include Makefile.macos
else ifeq ($(OPAL_OS),windows)
include Makefile.windows
else
$(error Unsupported platform '$(UNAME_S)'; OPAL currently supports Linux, macOS, and Windows)
endif

ifeq ($(OPAL_OS),windows)
TAILNET_SRCS := src/platform/windows/tailnet.cpp
else
TAILNET_SRCS := src/tailnet.cpp
LOCAL_DISCOVERY_SRCS += src/udp_socket_ops.cpp
APP_SRCS += src/udp_socket_ops.cpp
$(PRODUCT): src/udp_socket_ops.cpp
endif
APP_SRCS += $(TAILNET_SRCS)
$(PRODUCT): $(TAILNET_SRCS)

test-platform-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_platform_contract.cpp -o $(BUILD)/test-platform-contract
	$(BUILD)/test-platform-contract

test-cross-platform-architecture: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_cross_platform_architecture.cpp -o $(BUILD)/test-cross-platform-architecture
	$(BUILD)/test-cross-platform-architecture

test-udp-batch-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_udp_batch_contract.cpp -o $(BUILD)/test-udp-batch-contract
	$(BUILD)/test-udp-batch-contract

test-native-video-pipeline: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_native_video_pipeline.cpp src/native_video_pipeline.cpp -o $(BUILD)/test-native-video-pipeline
	$(BUILD)/test-native-video-pipeline

test-video-sender-encoder-control: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_video_sender_encoder_control.cpp -o $(BUILD)/test-video-sender-encoder-control
	$(BUILD)/test-video-sender-encoder-control

test-audio-capture-backend: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_audio_capture_backend.cpp -o $(BUILD)/test-audio-capture-backend
	$(BUILD)/test-audio-capture-backend

test-macos-media-scheduler: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_macos_media_scheduler.cpp -o $(BUILD)/test-macos-media-scheduler
	$(BUILD)/test-macos-media-scheduler

test-macos-clipboard-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_macos_clipboard_contract.cpp -o $(BUILD)/test-macos-clipboard-contract
	$(BUILD)/test-macos-clipboard-contract

test-macos-capture-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_macos_capture_contract.cpp -o $(BUILD)/test-macos-capture-contract
	$(BUILD)/test-macos-capture-contract

test-macos-videotoolbox-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_macos_videotoolbox_contract.cpp -o $(BUILD)/test-macos-videotoolbox-contract
	$(BUILD)/test-macos-videotoolbox-contract

test-macos-audio-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_macos_audio_contract.cpp -o $(BUILD)/test-macos-audio-contract
	$(BUILD)/test-macos-audio-contract

test-macos-host-permission-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_macos_host_permission_contract.cpp -o $(BUILD)/test-macos-host-permission-contract
	$(BUILD)/test-macos-host-permission-contract

test-platform-build-contract:
	MAKE_BIN='$(MAKE)' sh tests/test_platform_build_contract.sh

.PHONY: test-platform-contract test-cross-platform-architecture test-udp-batch-contract \
	test-native-video-pipeline test-video-sender-encoder-control test-audio-capture-backend \
	test-macos-media-scheduler test-macos-clipboard-contract test-macos-capture-contract \
	test-macos-videotoolbox-contract test-macos-audio-contract \
	test-macos-host-permission-contract test-platform-build-contract
endif
