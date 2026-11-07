/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                              \
			failures++;                                            \
		}                                                              \
	} while (0)

static const uint8_t addr[BGPD_ADDR_SIZE] = {0x55, 0x44, 0x33, 0x22,
					     0x11, 0x00, 0x00};

static void test_format(void)
{
	char text[BGPD_ADDR_TEXT];

	bgpd_address_format(addr, text);
	CHECK(strcmp(text, "00:11:22:33:44:55") == 0);
}

static void test_type_name(void)
{
	CHECK(strcmp(bgpd_address_type_name(BGPD_ADDR_BREDR), "BR") == 0);
	CHECK(strcmp(bgpd_address_type_name(BGPD_ADDR_LE_PUBLIC), "LE-P") == 0);
	CHECK(strcmp(bgpd_address_type_name(BGPD_ADDR_LE_RANDOM), "LE-R") == 0);
	CHECK(strcmp(bgpd_address_type_name(3), "?") == 0);
}

static void test_get(struct bgpd_devices *devices)
{
	struct bgpd_device *a, *b;
	uint8_t other[BGPD_ADDR_SIZE];

	a = bgpd_devices_get(devices, addr);
	CHECK(a != NULL && devices->count == 1);
	if (a == NULL)
		return;
	CHECK(strcmp(a->name, "00:11:22:33:44:55") == 0);
	CHECK(bgpd_devices_get(devices, addr) == a);
	CHECK(devices->count == 1);

	memcpy(other, addr, sizeof(other));
	other[6] = BGPD_ADDR_LE_PUBLIC;
	b = bgpd_devices_get(devices, other);
	CHECK(b != NULL && b != a && devices->count == 2);

	other[6] = 3;
	CHECK(bgpd_devices_get(devices, other) == NULL);
}

static void test_full(struct bgpd_devices *devices)
{
	uint8_t next[BGPD_ADDR_SIZE] = {0};

	devices->count = 0;
	for (unsigned int i = 0; i < BGPD_MAX_DEVICES; i++) {
		next[0] = (uint8_t)i;
		next[1] = (uint8_t)(i >> 8U);
		CHECK(bgpd_devices_get(devices, next) != NULL);
	}
	next[0] = 0xff;
	next[2] = 1;
	CHECK(bgpd_devices_get(devices, next) == NULL);
	CHECK(devices->count == BGPD_MAX_DEVICES);
}

int main(void)
{
	struct bgpd_devices *devices = calloc(1, sizeof(*devices));

	if (devices == NULL)
		return 1;
	test_format();
	test_type_name();
	test_get(devices);
	test_full(devices);
	free(devices);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
