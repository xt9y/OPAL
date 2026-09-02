CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDLIBS += -lssl -lcrypto -lX11 -lXi -lpthread
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec/opal
SYSTEMDUSERDIR ?= $(PREFIX)/lib/systemd/user
UDEVDIR ?= /usr/lib/udev/rules.d
INSTALL ?= install
BUILD := build
PRODUCT := $(BUILD)/opal
INPUT := $(BUILD)/opal-input
CORE_SRCS := src/config.cpp src/crypto.cpp src/media.cpp src/wake.cpp
NET_SRCS := src/net.cpp src/config.cpp src/crypto.cpp
INPUT_SRCS := src/input.cpp
APP_SRCS := src/main.cpp src/setup.cpp src/host.cpp src/client.cpp src/net.cpp src/tunnel.cpp src/system.cpp $(CORE_SRCS) $(INPUT_SRCS)

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

test-media: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_media.cpp src/media.cpp -o $(BUILD)/test-media
	$(BUILD)/test-media

test-input: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_input.cpp $(INPUT_SRCS) -o $(BUILD)/test-input
	$(BUILD)/test-input

test-setup: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_setup.cpp src/setup.cpp src/config.cpp -o $(BUILD)/test-setup
	$(BUILD)/test-setup

test-daemon: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_daemon.cpp -o $(BUILD)/test-daemon
	$(BUILD)/test-daemon

test-tunnel: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_tunnel.cpp src/tunnel.cpp src/config.cpp src/crypto.cpp -lssl -lcrypto -o $(BUILD)/test-tunnel
	$(BUILD)/test-tunnel

test-net: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_net.cpp $(NET_SRCS) -lssl -lcrypto -lpthread -o $(BUILD)/test-net
	$(BUILD)/test-net

test-clean: all
	BIN=$(abspath $(PRODUCT)) sh ./tests/test_clean.sh

test-install: all
	MAKE=$(MAKE) sh ./tests/test_install.sh

test: all test-core test-media test-input test-setup test-daemon test-tunnel test-net test-clean test-install
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
	@echo "Installed OPAL to $(DESTDIR)$(PREFIX)"
	@echo "Run: opal"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/opal" "$(DESTDIR)$(LIBEXECDIR)/opal-input"
	rm -f "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-host.service" "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-bridge.service"
	rm -f "$(DESTDIR)$(UDEVDIR)/70-opal-uinput.rules"
	-rmdir "$(DESTDIR)$(LIBEXECDIR)" 2>/dev/null

clean:
	rm -rf $(BUILD)

.PHONY: all test test-core test-media test-input test-setup test-daemon test-tunnel test-net test-clean test-install install uninstall clean
