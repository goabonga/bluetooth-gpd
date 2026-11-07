/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "device.h"

#include <stdio.h>
#include <string.h>

void bgpd_address_format(const uint8_t addr[BGPD_ADDR_SIZE],
			 char out[BGPD_ADDR_TEXT])
{
	snprintf(out, BGPD_ADDR_TEXT, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5],
		 addr[4], addr[3], addr[2], addr[1], addr[0]);
}

const char *bgpd_address_type_name(uint8_t type)
{
	switch (type) {
	case BGPD_ADDR_BREDR:
		return "BR";
	case BGPD_ADDR_LE_PUBLIC:
		return "LE-P";
	case BGPD_ADDR_LE_RANDOM:
		return "LE-R";
	default:
		return "?";
	}
}

struct bgpd_device *bgpd_devices_get(struct bgpd_devices *devices,
				     const uint8_t addr[BGPD_ADDR_SIZE])
{
	struct bgpd_device *device;

	if (addr[6] > BGPD_ADDR_LE_RANDOM)
		return NULL;
	for (size_t i = 0; i < devices->count; i++)
		if (memcmp(devices->items[i].addr, addr, BGPD_ADDR_SIZE) == 0)
			return &devices->items[i];
	if (devices->count == BGPD_MAX_DEVICES)
		return NULL;
	device = &devices->items[devices->count++];
	memset(device, 0, sizeof(*device));
	memcpy(device->addr, addr, BGPD_ADDR_SIZE);
	bgpd_address_format(addr, device->name);
	return device;
}
