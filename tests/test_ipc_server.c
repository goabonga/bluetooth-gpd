/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

static struct bgpd_ipc_command received[8];
static size_t received_count;

static void handler(void *context, const struct bgpd_ipc_command *command)
{
	(void)context;
	if (received_count < sizeof(received) / sizeof(received[0]))
		received[received_count++] = *command;
}

static int connect_to(const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (fd >= 0 &&
	    connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* What the client received, once the server had a chance to answer. */
static size_t receive(int fd, char *text, size_t size)
{
	size_t used = 0;
	struct pollfd p = {.fd = fd, .events = POLLIN};

	while (used + 1 < size && poll(&p, 1, 100) == 1) {
		ssize_t n = read(fd, text + used, size - used - 1);

		if (n <= 0)
			break;
		used += (size_t)n;
	}
	text[used] = '\0';
	return used;
}

static void serve(struct bgpd_ipc_server *server, const struct bgpd_mgmt *m)
{
	bgpd_ipc_serve(server, m, handler, NULL);
}

int main(void)
{
	struct bgpd_ipc_server *server = calloc(1, sizeof(*server));
	struct bgpd_ipc_server *other = calloc(1, sizeof(*other));
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_mgmt *mirror = calloc(1, sizeof(*mirror));
	char *text = calloc(1, BGPD_IPC_SNAPSHOT_MAX);
	char dir[] = "/tmp/bgpd-ipc-XXXXXX";
	char path[128], line[64];
	struct stat st;
	int a, b;

	if (server == NULL || other == NULL || m == NULL || mirror == NULL ||
	    text == NULL || mkdtemp(dir) == NULL) {
		failures++;
		goto cleanup;
	}
	snprintf(path, sizeof(path), "%s/bluetooth.sock", dir);
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_mgmt_init(mirror, BGPD_NO_INDEX, NULL);
	m->ready = true;
	snprintf(m->status, sizeof(m->status), "Ready");

	CHECK(bgpd_ipc_listen(server, path) == 0);
	CHECK(stat(path, &st) == 0 && S_ISSOCK(st.st_mode));
	CHECK((st.st_mode & 0777U) == 0600U);
	CHECK(bgpd_ipc_listen(other, path) == -1 && errno == EADDRINUSE);

	/* A client receives the state when it connects. */
	a = connect_to(path);
	CHECK(a >= 0);
	serve(server, m);
	CHECK(bgpd_ipc_clients(server) == 1);
	CHECK(receive(a, text, BGPD_IPC_SNAPSHOT_MAX) > 0);
	CHECK(bgpd_ipc_apply(mirror, text) == 0);
	CHECK(mirror->ready && strcmp(mirror->status, "Ready") == 0);

	/* Nothing is sent while the state does not change. */
	serve(server, m);
	CHECK(receive(a, text, BGPD_IPC_SNAPSHOT_MAX) == 0);

	/* A change is pushed. */
	snprintf(m->status, sizeof(m->status), "Scanning");
	serve(server, m);
	CHECK(receive(a, text, BGPD_IPC_SNAPSHOT_MAX) > 0);
	CHECK(bgpd_ipc_apply(mirror, text) == 0);
	CHECK(strcmp(mirror->status, "Scanning") == 0);

	/* Commands reach the handler; an invalid line is ignored. */
	{
		struct bgpd_ipc_command pair = {.kind = BGPD_IPC_PAIR};

		memcpy(pair.addr, remote, BGPD_ADDR_SIZE);
		CHECK(write(a, "scan\nbogus\n", 11) == 11);
		CHECK(bgpd_ipc_format(&pair, line, sizeof(line)) > 0);
		CHECK(write(a, line, strlen(line)) == (ssize_t)strlen(line));
	}
	serve(server, m);
	CHECK(received_count == 2);
	CHECK(received[0].kind == BGPD_IPC_SCAN);
	CHECK(received[1].kind == BGPD_IPC_PAIR);
	CHECK(memcmp(received[1].addr, remote, BGPD_ADDR_SIZE) == 0);

	/* A new client gets the current state, and both see the changes. */
	b = connect_to(path);
	serve(server, m);
	CHECK(bgpd_ipc_clients(server) == 2);
	CHECK(receive(b, text, BGPD_IPC_SNAPSHOT_MAX) > 0);
	CHECK(bgpd_ipc_apply(mirror, text) == 0);
	CHECK(strcmp(mirror->status, "Scanning") == 0);

	/* Clients leave. */
	close(a);
	serve(server, m);
	CHECK(bgpd_ipc_clients(server) == 1);

	/* A line without end drops its client. */
	memset(text, 'x', 300);
	CHECK(write(b, text, 300) == 300);
	serve(server, m);
	CHECK(bgpd_ipc_clients(server) == 0);
	close(b);

	/* A socket left by a stopped daemon is replaced. */
	close(server->listener);
	server->listener = -1;
	CHECK(bgpd_ipc_listen(other, path) == 0);
	bgpd_ipc_server_close(other);
	CHECK(stat(path, &st) == -1 && errno == ENOENT);

	bgpd_ipc_server_close(server);
	rmdir(dir);
cleanup:
	free(text);
	free(mirror);
	free(m);
	free(other);
	free(server);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
