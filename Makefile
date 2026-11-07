# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -Iinclude
PREFIX  ?= /usr/local
SANITIZE ?= address,undefined
SOURCES := $(wildcard src/*.c include/*.h tests/*.c)
BUILD   := build

CORE    := bgpd device store mgmt
CORE_C  := $(CORE:%=src/%.c)
LIB_OBJ := $(CORE:%=$(BUILD)/%.o)
TESTS   := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: help all test format format-check lint clean install

.DEFAULT_GOAL := all

help: ## List the make targets
	@awk -F':.*## ' '/^[a-z][a-z-]*:.*## /{printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

all: $(BUILD)/bluetooth ## Build everything

$(BUILD)/bluetooth: $(BUILD)/main.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Unit tests, built with the address and undefined behaviour sanitizers.
# Disable them with `make test SANITIZE=` where the platform rejects them.
test: $(TESTS) ## Build and run the unit tests
	@for t in $(TESTS); do echo $$t; $$t || exit 1; done

$(BUILD)/test_%: tests/test_%.c $(CORE_C) $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -g $(if $(SANITIZE),-fsanitize=$(SANITIZE) -fno-sanitize-recover=all) \
		$(LDFLAGS) -o $@ $< $(CORE_C)

format: ## Format the sources with clang-format
	clang-format -i $(SOURCES)

format-check: ## Fail when a source is not formatted
	clang-format --dry-run -Werror $(SOURCES)

lint: ## Run clang-tidy on the sources
	clang-tidy --quiet $(filter %.c,$(SOURCES)) -- -std=c11 -D_GNU_SOURCE -Iinclude

install: all ## Install under PREFIX, honouring DESTDIR
	install -Dm755 $(BUILD)/bluetooth $(DESTDIR)$(PREFIX)/bin/bluetooth

clean: ## Remove the build directory
	rm -rf $(BUILD)
