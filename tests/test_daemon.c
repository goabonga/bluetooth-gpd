/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void command(struct bgpd_ipc_client *client, struct bgpd_daemon *daemon,
		    struct bgpd_mgmt *mirror, enum bgpd_ipc_kind kind,
		    const uint8_t *addr)
{
	struct bgpd_ipc_command c = {.kind = kind};

	if (addr != NULL)
		memcpy(c.addr, addr, BGPD_ADDR_SIZE);
	CHECK(bgpd_ipc_send(client, &c) == 0);
	for (int i = 0; i < 5; i++) {
		usleep(5000);
		bgpd_daemon_poll(daemon);
		bgpd_ipc_poll(client, mirror);
	}
}

static void test_interface(void)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_mgmt *mirror = calloc(1, sizeof(*mirror));
	struct bgpd_daemon *daemon = calloc(1, sizeof(*daemon));
	struct bgpd_ipc_client *client = calloc(1, sizeof(*client));
	char dir[] = "/tmp/bgpd-daemon-XXXXXX";
	char path[128];
	struct bgpd_daemon_slot *slot;
	struct bgpd_device *d;
	uint8_t addr[BGPD_ADDR_SIZE];

	if (m == NULL || mirror == NULL || daemon == NULL || client == NULL ||
	    mkdtemp(dir) == NULL) {
		CHECK(!"memory");
		goto cleanup;
	}
	snprintf(path, sizeof(path), "%s/bluetooth.sock", dir);
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_mgmt_init(mirror, BGPD_NO_INDEX, NULL);
	bgpd_daemon_init(daemon, m, NULL);
	m->ready = true;
	d = add(m, 1, BGPD_ADDR_LE_PUBLIC, true);
	if (d == NULL) {
		CHECK(!"device");
		goto cleanup;
	}
	memcpy(addr, d->addr, BGPD_ADDR_SIZE);
	add(m, 2, BGPD_ADDR_LE_PUBLIC, false);

	CHECK(bgpd_daemon_listen(daemon, path) == 0);
	bgpd_ipc_connect(client, path);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, addr);

	/* The interface sees the devices of the daemon. */
	CHECK(mirror->ready && mirror->devices.count == 2);
	CHECK(used_slots(daemon) == 1);
	slot = &daemon->slots[0];

	/* A disconnection holds the device until a connection is asked. */
	command(client, daemon, mirror, BGPD_IPC_DISCONNECT, addr);
	CHECK(slot->held && !bgpd_hog_active(&slot->hog));
	slot->retry = 0;
	bgpd_daemon_poll(daemon);
	CHECK(!bgpd_hog_active(&slot->hog));
	command(client, daemon, mirror, BGPD_IPC_CONNECT, addr);
	CHECK(!slot->held);

	/* The adapter is made visible on request. */
	m->settings = 1U;
	command(client, daemon, mirror, BGPD_IPC_VISIBLE, NULL);
	CHECK(m->prepare_action == BGPD_OP_DISCOVERABLE);

	/* A device that is not paired is not connected. */
	addr[0] = 2;
	command(client, daemon, mirror, BGPD_IPC_CONNECT, addr);
	CHECK(used_slots(daemon) == 1);
	CHECK(strstr(mirror->status, "Pair the device first") != NULL);

	/* Forgetting a device frees its slot once it is unpaired. */
	addr[0] = 1;
	command(client, daemon, mirror, BGPD_IPC_UNPAIR, addr);
	CHECK(slot->held);
	d->paired = false;
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 0);

	bgpd_ipc_client_close(client);
cleanup:
	if (daemon != NULL)
		bgpd_daemon_close(daemon);
	rmdir(dir);
	free(client);
	free(daemon);
	free(mirror);
	free(m);
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

	/* The status of the manager is logged once, and a message the daemon
	 * wrote itself is not written again. */
	{
		char *text = NULL;
		size_t size = 0;
		FILE *log = open_memstream(&text, &size);

		CHECK(log != NULL);
		daemon->log = log;
		daemon->logged[0] = '\0';
		snprintf(m->status, sizeof(m->status), "Hello");
		bgpd_daemon_log_changes(daemon);
		bgpd_daemon_log_changes(daemon);
		snprintf(m->status, sizeof(m->status), "World");
		bgpd_daemon_log_changes(daemon);
		fflush(log);
		CHECK(text != NULL && strcmp(text, "Hello\nWorld\n") == 0);
		daemon->log = NULL;
		fclose(log);
		free(text);
	}

	bgpd_daemon_close(daemon);
	free(daemon);
	free(m);
	test_interface();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
