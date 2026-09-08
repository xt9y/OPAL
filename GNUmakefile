UNAME_S := $(shell uname -s)
OPAL_OS ?= $(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))

ifeq ($(UNAME_S),Darwin)
ifeq ($(filter oneshell,$(.FEATURES)),)
$(error OPAL macOS requires GNU Make 3.82+ because the build uses .ONESHELL. Install it with 'brew install make' and run 'gmake' instead of Apple's /usr/bin/make)
endif
endif

ifeq ($(OPAL_OS),linux)
include Makefile
else ifeq ($(OPAL_OS),macos)
include Makefile.macos
else
$(error Unsupported platform '$(UNAME_S)'; OPAL currently supports Linux and macOS)
endif

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
