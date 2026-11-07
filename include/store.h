/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#ifndef BGPD_STORE_H
#define BGPD_STORE_H

#include "device.h"

#define BGPD_MAX_KEYS 512
#define BGPD_KEY_DATA_SIZE 36
#define BGPD_STORE_FILE_SIZE 64

/* Kinds of keys, as loaded into the kernel by the management API. */
enum bgpd_key_kind {
	BGPD_KEY_LINK = 1, /* BR/EDR link key, 25 bytes */
	BGPD_KEY_LTK = 2,  /* LE long term key, 36 bytes */
	BGPD_KEY_IRK = 3,  /* LE identity resolving key, 23 bytes */
};

/* The data starts with the address (six bytes and the type). */
struct bgpd_key {
	uint8_t kind, length, data[BGPD_KEY_DATA_SIZE];
};

/*
 * Pairing keys of one controller, kept in a private directory (mode 0700,
 * owned by the user, locked while open) in a file named after the
 * controller. Every function returns 0, or -1 with errno set.
 */
struct bgpd_store {
	int dirfd, lockfd;
	char file[BGPD_STORE_FILE_SIZE];
	struct bgpd_key keys[BGPD_MAX_KEYS];
	size_t count;
};

/* $XDG_STATE_HOME/bluetooth, else ~/.local/state/bluetooth. */
int bgpd_store_default_dir(char *buffer, size_t size);

/* Initialises store, creates dir if needed and locks it. */
int bgpd_store_open(struct bgpd_store *store, const char *dir);

/* Selects the key file and loads it; a missing file is an empty store. */
int bgpd_store_load(struct bgpd_store *store, const char *file);

/* Flags the devices holding a link or long term key as paired and saved. */
void bgpd_store_mark(const struct bgpd_store *store,
		     struct bgpd_devices *devices);

/* Adds or replaces a key and saves the file. EINVAL if it is malformed. */
int bgpd_store_add(struct bgpd_store *store, uint8_t kind, const uint8_t *data,
		   size_t length);

/* Drops every key of the device and saves the file. */
int bgpd_store_forget(struct bgpd_store *store,
		      const uint8_t addr[BGPD_ADDR_SIZE]);

void bgpd_store_close(struct bgpd_store *store);

#endif /* BGPD_STORE_H */
