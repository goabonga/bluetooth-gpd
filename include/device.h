/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#ifndef BGPD_DEVICE_H
#define BGPD_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BGPD_MAX_DEVICES 512
#define BGPD_ADDR_SIZE 7  /* six little-endian bytes, then the type */
#define BGPD_ADDR_TEXT 18 /* "AA:BB:CC:DD:EE:FF" and the NUL */
#define BGPD_NAME_SIZE 249

enum bgpd_addr_type {
	BGPD_ADDR_BREDR = 0,
	BGPD_ADDR_LE_PUBLIC = 1,
	BGPD_ADDR_LE_RANDOM = 2,
};

struct bgpd_device {
	uint8_t addr[BGPD_ADDR_SIZE];
	char name[BGPD_NAME_SIZE];
	int rssi;
	bool has_rssi, paired, saved, connected;
};

struct bgpd_devices {
	struct bgpd_device items[BGPD_MAX_DEVICES];
	size_t count;
};

/* Writes the address in the usual most-significant-byte-first form. */
void bgpd_address_format(const uint8_t addr[BGPD_ADDR_SIZE],
			 char out[BGPD_ADDR_TEXT]);

/* "BR", "LE-P" or "LE-R"; "?" for an unknown type. */
const char *bgpd_address_type_name(uint8_t type);

/*
 * Returns the entry for addr, creating it (named after its address) when
 * it is new. Identical addresses of different types are separate entries.
 * Returns NULL for an unknown address type or when the table is full.
 */
struct bgpd_device *bgpd_devices_get(struct bgpd_devices *devices,
				     const uint8_t addr[BGPD_ADDR_SIZE]);

#endif /* BGPD_DEVICE_H */
