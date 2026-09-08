UNAME_S := $(shell uname -s)
OPAL_OS ?= $(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))

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

test-udp-batch-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_udp_batch_contract.cpp -o $(BUILD)/test-udp-batch-contract
	$(BUILD)/test-udp-batch-contract

.PHONY: test-platform-contract test-udp-batch-contract
