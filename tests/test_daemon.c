/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "daemon.h"

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

static size_t used_slots(const struct bgpd_daemon *daemon)
{
	size_t n = 0;

	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		if (daemon->slots[i].used)
			n++;
	return n;
}

static struct bgpd_device *add(struct bgpd_mgmt *m, uint8_t last, uint8_t type,
			       bool paired)
{
	const uint8_t addr[BGPD_ADDR_SIZE] = {last, 2, 3, 4, 5, 6, type};
	struct bgpd_device *d = bgpd_devices_get(&m->devices, addr);

	if (d != NULL)
		d->paired = paired;
	return d;
}

int main(void)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_daemon *daemon = calloc(1, sizeof(*daemon));

	if (m == NULL || daemon == NULL) {
		free(daemon);
		free(m);
		return 1;
	}
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_daemon_init(daemon, m, NULL);

	add(m, 1, BGPD_ADDR_LE_PUBLIC, true);
	add(m, 2, BGPD_ADDR_BREDR, true);
	add(m, 3, BGPD_ADDR_LE_RANDOM, false);

	/* Nothing is connected before the adapter is known. */
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 0);

	/* Only the paired LE device gets a connection. */
	m->ready = true;
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 1);
	CHECK(daemon->slots[0].retry > 0);

	/* The same device keeps its slot and is not retried at once. */
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 1);

	/* Another paired device takes the next slot. */
	add(m, 4, BGPD_ADDR_LE_RANDOM, true);
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 2);
	CHECK(bgpd_daemon_running(daemon) == 0);

	bgpd_daemon_close(daemon);
	free(daemon);
	free(m);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
