# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
PREFIX  ?= /usr/local
SANITIZE ?= address,undefined
SOURCES := $(wildcard src/*.c include/*.h tests/*.c)
BUILD   := build

LIB_OBJ := $(BUILD)/bgpd.o

.PHONY: help all test format format-check lint clean install

.DEFAULT_GOAL := all

help: ## List the make targets
	@awk -F':.*## ' '/^[a-z][a-z-]*:.*## /{printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

all: $(BUILD)/bluetooth ## Build everything

$(BUILD)/bluetooth: $(BUILD)/main.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c include/bgpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Unit tests, built with the address and undefined behaviour sanitizers.
# Disable them with `make test SANITIZE=` where the platform rejects them.
test: $(BUILD)/test_bgpd ## Build and run the unit tests
	$(BUILD)/test_bgpd

$(BUILD)/test_bgpd: tests/test_bgpd.c src/bgpd.c include/bgpd.h | $(BUILD)
	$(CC) $(CFLAGS) -g $(if $(SANITIZE),-fsanitize=$(SANITIZE) -fno-sanitize-recover=all) \
		$(LDFLAGS) -o $@ tests/test_bgpd.c src/bgpd.c

format: ## Format the sources with clang-format
	clang-format -i $(SOURCES)

format-check: ## Fail when a source is not formatted
	clang-format --dry-run -Werror $(SOURCES)

lint: ## Run clang-tidy on the sources
	clang-tidy --quiet $(filter %.c,$(SOURCES)) -- -std=c11 -Iinclude

install: all ## Install under PREFIX, honouring DESTDIR
	install -Dm755 $(BUILD)/bluetooth $(DESTDIR)$(PREFIX)/bin/bluetooth

clean: ## Remove the build directory
	rm -rf $(BUILD)
