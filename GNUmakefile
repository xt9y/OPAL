UNAME_S := $(shell uname -s)
OPAL_OS ?= $(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))

ifeq ($(OPAL_OS),linux)
include Makefile
else ifeq ($(OPAL_OS),macos)
include Makefile.macos
else
$(error Unsupported platform '$(UNAME_S)'; OPAL currently supports Linux and macOS)
endif
