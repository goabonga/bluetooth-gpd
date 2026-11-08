# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -Iinclude
PREFIX  ?= /usr/local
PKG_CONFIG ?= pkg-config
NCURSES_CFLAGS := $(shell $(PKG_CONFIG) --cflags ncursesw)
NCURSES_LIBS   := $(shell $(PKG_CONFIG) --libs ncursesw)

# ctui provides the interface components. CTUI_DIR=path (command line or
# environment) selects that checkout, built on demand, to develop against a
# local ctui; otherwise an installed ctui.pc is used, else the sibling
# checkout ../ctui.
CTUI_DIR ?= ../ctui
ifneq ($(origin CTUI_DIR),file)
CTUI_LOCAL := yes
else ifneq ($(shell $(PKG_CONFIG) --exists ctui && echo yes),yes)
CTUI_LOCAL := yes
endif
ifdef CTUI_LOCAL
CTUI_CFLAGS := -I$(CTUI_DIR)/include $(NCURSES_CFLAGS)
CTUI_LIBS   := $(CTUI_DIR)/build/libctui.a -lm $(NCURSES_LIBS)
CTUI_DEP    := $(CTUI_DIR)/build/libctui.a
else
CTUI_CFLAGS := $(shell $(PKG_CONFIG) --cflags ctui) $(NCURSES_CFLAGS)
CTUI_LIBS   := $(shell $(PKG_CONFIG) --libs ctui) $(NCURSES_LIBS)
CTUI_DEP    :=
endif
SANITIZE ?= address,undefined
SOURCES := $(wildcard src/*.c include/*.h tests/*.c)
BUILD   := build

CORE    := device store mgmt hog daemon
CORE_C  := $(CORE:%=src/%.c)
LIB_OBJ := $(CORE:%=$(BUILD)/%.o)
LIB     := $(BUILD)/libbgpd.a
TESTS   := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: help all test format format-check lint clean install install-capabilities

.DEFAULT_GOAL := all

help: ## List the make targets
	@awk -F':.*## ' '/^[a-z][a-z-]*:.*## /{printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

all: $(BUILD)/bluetooth $(BUILD)/bluetooth-daemon ## Build everything

# The core modules form a static library, shared by every executable.
$(LIB): $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/bluetooth: $(BUILD)/main.o $(BUILD)/ui.o $(LIB) $(CTUI_DEP)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/main.o $(BUILD)/ui.o $(LIB) $(CTUI_LIBS)

$(BUILD)/bluetooth-daemon: $(BUILD)/daemon_main.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/daemon_main.o $(LIB) -lutil

$(BUILD)/%.o: src/%.c $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(CTUI_CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

$(CTUI_DIR)/build/libctui.a:
	$(MAKE) -C $(CTUI_DIR) build/libctui.a

# Unit tests, built with the address and undefined behaviour sanitizers.
# -fno-pie -no-pie: on linux-hardened a PIE lands inside the ASan shadow
# range. Disable the sanitizers with `make test SANITIZE=` where the
# platform rejects them.
test: all $(TESTS) ## Build and run the unit tests
	@for t in $(TESTS); do echo $$t; $$t $(BUILD)/bluetooth || exit 1; done

$(BUILD)/test_%: tests/test_%.c $(CORE_C) $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -g -fno-pie -no-pie $(if $(SANITIZE),-fsanitize=$(SANITIZE) -fno-sanitize-recover=all) \
		$(LDFLAGS) -o $@ $< $(CORE_C) -lutil

format: ## Format the sources with clang-format
	clang-format -i $(SOURCES)

format-check: ## Fail when a source is not formatted
	clang-format --dry-run -Werror $(SOURCES)

lint: $(CTUI_DEP) ## Run clang-tidy on the sources
	clang-tidy --quiet $(filter %.c,$(SOURCES)) -- -std=c11 -D_GNU_SOURCE -Iinclude $(CTUI_CFLAGS)

install: all ## Install under PREFIX, honouring DESTDIR
	install -Dm755 $(BUILD)/bluetooth $(DESTDIR)$(PREFIX)/bin/bluetooth
	install -Dm755 $(BUILD)/bluetooth-daemon $(DESTDIR)$(PREFIX)/bin/bluetooth-daemon
	sed 's|@BINDIR@|$(PREFIX)/bin|' systemd/bluetooth-daemon.service.in > $(BUILD)/bluetooth-daemon.service
	install -Dm644 $(BUILD)/bluetooth-daemon.service $(DESTDIR)$(PREFIX)/lib/systemd/user/bluetooth-daemon.service
	install -Dm644 udev/70-bluetooth-uhid.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/70-bluetooth-uhid.rules

# Lets the daemon open the management socket without running as root. It
# drops the capability as soon as the socket is bound.
install-capabilities: ## Grant the installed daemon CAP_NET_ADMIN
	setcap cap_net_admin+ep $(DESTDIR)$(PREFIX)/bin/bluetooth-daemon

clean: ## Remove the build directory
	rm -rf $(BUILD)
