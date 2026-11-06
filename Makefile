# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
PREFIX  ?= /usr/local
BUILD   := build

LIB_OBJ := $(BUILD)/bgpd.o

.PHONY: all test clean install

all: $(BUILD)/bluetooth

$(BUILD)/bluetooth: $(BUILD)/main.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c include/bgpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Unit tests, built with the address and undefined behaviour sanitizers.
test: $(BUILD)/test_bgpd
	$(BUILD)/test_bgpd

$(BUILD)/test_bgpd: tests/test_bgpd.c src/bgpd.c include/bgpd.h | $(BUILD)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		$(LDFLAGS) -o $@ tests/test_bgpd.c src/bgpd.c

install: all
	install -Dm755 $(BUILD)/bluetooth $(DESTDIR)$(PREFIX)/bin/bluetooth

clean:
	rm -rf $(BUILD)
