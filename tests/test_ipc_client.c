/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ipc.h"

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

static const uint8_t remote[BGPD_ADDR_SIZE] = {
    0x55, 0x44, 0x33, 0x22, 0x11, 0x00, BGPD_ADDR_LE_PUBLIC};

static struct bgpd_ipc_command received;
static size_t received_count;

static void handler(void *context, const struct bgpd_ipc_command *command)
{
	(void)context;
	received = *command;
	received_count++;
}

/* Runs both sides until the client's mirror satisfies done(), or times out. */
static bool settle(struct bgpd_ipc_server *server, const struct bgpd_mgmt *m,
		   struct bgpd_ipc_client *client, struct bgpd_mgmt *mirror,
		   bool (*done)(const struct bgpd_mgmt *))
{
	for (int i = 0; i < 300; i++) {
		if (server != NULL)
			bgpd_ipc_serve(server, m, handler, NULL);
		bgpd_ipc_poll(client, mirror);
		if (done(mirror))
			return true;
		usleep(10000);
	}
	return false;
}

static bool is_scanning(const struct bgpd_mgmt *mirror)
{
	return mirror->ready && strcmp(mirror->status, "Scanning") == 0;
}

static bool is_waiting(const struct bgpd_mgmt *mirror)
{
	return !mirror->ready && strstr(mirror->status, "daemon") != NULL;
}

static bool has_device(const struct bgpd_mgmt *mirror)
{
	return mirror->ready && mirror->devices.count == 1;
}

int main(void)
{
	struct bgpd_ipc_server *server = calloc(1, sizeof(*server));
	struct bgpd_ipc_client *client = calloc(1, sizeof(*client));
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_mgmt *mirror = calloc(1, sizeof(*mirror));
	char dir[] = "/tmp/bgpd-ipc-XXXXXX";
	char path[128];
	struct bgpd_ipc_command command = {.kind = BGPD_IPC_UNPAIR};

	if (server == NULL || client == NULL || m == NULL || mirror == NULL ||
	    mkdtemp(dir) == NULL) {
		failures++;
		goto cleanup;
	}
	snprintf(path, sizeof(path), "%s/bluetooth.sock", dir);
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_mgmt_init(mirror, BGPD_NO_INDEX, NULL);
	m->ready = true;
	snprintf(m->status, sizeof(m->status), "Scanning");
	bgpd_devices_get(&m->devices, remote);

	/* Without a daemon the mirror says so, and commands fail. */
	bgpd_ipc_connect(client, path);
	CHECK(!bgpd_ipc_poll(client, mirror));
	CHECK(is_waiting(mirror));
	CHECK(bgpd_ipc_send(client, &command) == -1);

	/* The client finds the daemon once it runs. */
	CHECK(bgpd_ipc_listen(server, path) == 0);
	CHECK(settle(server, m, client, mirror, has_device));
	CHECK(is_scanning(mirror));
	CHECK(mirror->devices.count == 1);

	/* Commands reach the daemon. */
	memcpy(command.addr, remote, BGPD_ADDR_SIZE);
	CHECK(bgpd_ipc_send(client, &command) == 0);
	for (int i = 0; i < 100 && received_count == 0; i++) {
		bgpd_ipc_serve(server, m, handler, NULL);
		usleep(10000);
	}
	CHECK(received_count == 1 && received.kind == BGPD_IPC_UNPAIR);
	CHECK(memcmp(received.addr, remote, BGPD_ADDR_SIZE) == 0);

	/* A large state is received whole. */
	for (unsigned int i = 1; i < 300; i++) {
		uint8_t addr[BGPD_ADDR_SIZE] = {
		    (uint8_t)i, (uint8_t)(i >> 8U), 3, 4, 5, 6, 0};
		struct bgpd_device *d = bgpd_devices_get(&m->devices, addr);

		if (d != NULL)
			snprintf(d->name, sizeof(d->name), "Device number %u",
				 i);
	}
	for (int i = 0; i < 300 && mirror->devices.count != m->devices.count;
	     i++) {
		bgpd_ipc_serve(server, m, handler, NULL);
		bgpd_ipc_poll(client, mirror);
		usleep(10000);
	}
	CHECK(mirror->devices.count == 300);

	/* When the daemon stops the mirror is marked, then it reconnects. */
	bgpd_ipc_server_close(server);
	CHECK(settle(NULL, m, client, mirror, is_waiting));
	CHECK(bgpd_ipc_listen(server, path) == 0);
	CHECK(settle(server, m, client, mirror, is_scanning));

	bgpd_ipc_client_close(client);
	bgpd_ipc_server_close(server);
	rmdir(dir);
cleanup:
	free(mirror);
	free(m);
	free(client);
	free(server);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
