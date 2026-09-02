CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDLIBS += -lssl -lcrypto -lX11 -lpthread
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
APP_SRCS := src/main.cpp src/host.cpp src/client.cpp src/net.cpp src/tunnel.cpp src/system.cpp $(CORE_SRCS)

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

test: all test-core
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
	@echo "Run: opal init && opal host setup"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/opal" "$(DESTDIR)$(LIBEXECDIR)/opal-input"
	rm -f "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-host.service" "$(DESTDIR)$(SYSTEMDUSERDIR)/opal-bridge.service"
	rm -f "$(DESTDIR)$(UDEVDIR)/70-opal-uinput.rules"
	-rmdir "$(DESTDIR)$(LIBEXECDIR)" 2>/dev/null

clean:
	rm -rf $(BUILD)

.PHONY: all test test-core install uninstall clean
