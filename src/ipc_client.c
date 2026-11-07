/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void disconnect(struct bgpd_ipc_client *client)
{
	if (client->fd >= 0)
		close(client->fd);
	client->fd = -1;
	client->used = 0;
}

static void try_connect(struct bgpd_ipc_client *client)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd;

	client->retry = bgpd_now() + BGPD_IPC_RECONNECT_MS;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s",
		 client->path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		return;
	}
	client->fd = fd;
	client->used = 0;
}

void bgpd_ipc_connect(struct bgpd_ipc_client *client, const char *path)
{
	memset(client, 0, sizeof(*client));
	client->fd = -1;
	snprintf(client->path, sizeof(client->path), "%s", path);
	try_connect(client);
}

static void lost(struct bgpd_ipc_client *client, struct bgpd_mgmt *mirror)
{
	disconnect(client);
	mirror->ready = false;
	mirror->scanning = mirror->pairing = false;
	mirror->command = 0;
	mirror->prompt_kind = BGPD_PROMPT_NONE;
	mirror->prompt[0] = '\0';
	snprintf(mirror->status, sizeof(mirror->status),
		 "Waiting for the daemon (systemctl --user start "
		 "bluetooth-daemon)");
}

/* Length of the first complete snapshot of the buffer, or 0. */
static size_t complete(const struct bgpd_ipc_client *client)
{
	for (size_t i = 0; i + 4 <= client->used; i++)
		if ((i == 0 || client->buffer[i - 1] == '\n') &&
		    memcmp(client->buffer + i, "end\n", 4) == 0)
			return i + 4;
	return 0;
}

bool bgpd_ipc_poll(struct bgpd_ipc_client *client, struct bgpd_mgmt *mirror)
{
	if (client->fd < 0) {
		if (bgpd_now() >= client->retry)
			try_connect(client);
		if (client->fd < 0) {
			if (mirror->ready || mirror->status[0] == '\0')
				lost(client, mirror);
			return false;
		}
	}
	for (;;) {
		size_t size;
		ssize_t n = read(client->fd, client->buffer + client->used,
				 BGPD_IPC_SNAPSHOT_MAX - client->used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			lost(client, mirror);
			return false;
		}
		client->used += (size_t)n;
		while ((size = complete(client)) > 0) {
			char kept = client->buffer[size];

			client->buffer[size] = '\0';
			if (bgpd_ipc_apply(mirror, client->buffer) < 0) {
				lost(client, mirror);
				return false;
			}
			client->buffer[size] = kept;
			memmove(client->buffer, client->buffer + size,
				client->used - size);
			client->used -= size;
		}
		if (client->used >= BGPD_IPC_SNAPSHOT_MAX) {
			lost(client, mirror);
			return false;
		}
	}
	return true;
}

int bgpd_ipc_send(struct bgpd_ipc_client *client,
		  const struct bgpd_ipc_command *command)
{
	char line[BGPD_IPC_LINE_MAX];
	size_t size = bgpd_ipc_format(command, line, sizeof(line));

	if (client->fd < 0 || size == 0) {
		errno = client->fd < 0 ? ENOTCONN : EINVAL;
		return -1;
	}
	if (send(client->fd, line, size, MSG_NOSIGNAL | MSG_DONTWAIT) !=
	    (ssize_t)size) {
		disconnect(client);
		return -1;
	}
	return 0;
}

void bgpd_ipc_client_close(struct bgpd_ipc_client *client)
{
	disconnect(client);
}
