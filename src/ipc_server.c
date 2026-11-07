/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int bgpd_ipc_default_path(char *buffer, size_t size)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int length;

	if (dir == NULL || dir[0] != '/') {
		errno = ENOENT;
		return -1;
	}
	length = snprintf(buffer, size, "%s/bluetooth.sock", dir);
	if (length < 0 || (size_t)length >= size ||
	    (size_t)length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int bind_socket(int fd, const struct sockaddr_un *address)
{
	return bind(fd, (const struct sockaddr *)address, sizeof(*address));
}

/* True when a daemon answers on the socket. */
static bool answers(const struct sockaddr_un *address)
{
	int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	bool alive;

	if (probe < 0)
		return false;
	alive = connect(probe, (const struct sockaddr *)address,
			sizeof(*address)) == 0;
	close(probe);
	return alive;
}

int bgpd_ipc_listen(struct bgpd_ipc_server *server, const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd, saved;

	memset(server, 0, sizeof(*server));
	server->listener = -1;
	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		server->peers[i].fd = -1;
	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(address.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (bind_socket(fd, &address) < 0) {
		if (errno != EADDRINUSE)
			goto fail;
		if (answers(&address)) {
			errno = EADDRINUSE;
			goto fail;
		}
		unlink(path);
		if (bind_socket(fd, &address) < 0)
			goto fail;
	}
	if (chmod(path, S_IRUSR | S_IWUSR) < 0 || listen(fd, 4) < 0) {
		saved = errno;
		unlink(path);
		errno = saved;
		goto fail;
	}
	server->listener = fd;
	snprintf(server->path, sizeof(server->path), "%s", path);
	return 0;
fail:
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}

static void drop(struct bgpd_ipc_peer *peer)
{
	if (peer->fd >= 0)
		close(peer->fd);
	peer->fd = -1;
	peer->used = 0;
}

static void send_snapshot(struct bgpd_ipc_peer *peer, const char *text,
			  size_t size)
{
	ssize_t n = send(peer->fd, text, size, MSG_NOSIGNAL | MSG_DONTWAIT);

	/* A partial snapshot would corrupt the stream: the client reconnects.
	 */
	if (n != (ssize_t)size)
		drop(peer);
}

static void accept_clients(struct bgpd_ipc_server *server)
{
	for (;;) {
		struct ucred cred;
		socklen_t length = sizeof(cred);
		struct bgpd_ipc_peer *slot = NULL;
		int fd = accept4(server->listener, NULL, NULL,
				 SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (fd < 0 && errno == EINTR)
			continue;
		if (fd < 0)
			return;
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) <
			0 ||
		    cred.uid != geteuid()) {
			close(fd);
			continue;
		}
		for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
			if (server->peers[i].fd < 0) {
				slot = &server->peers[i];
				break;
			}
		if (slot == NULL) {
			close(fd);
			continue;
		}
		slot->fd = fd;
		slot->used = 0;
		if (server->last_size > 0)
			send_snapshot(slot, server->last, server->last_size);
	}
}

static void read_commands(struct bgpd_ipc_peer *peer, bgpd_ipc_handler handler,
			  void *context)
{
	char buffer[256];

	while (peer->fd >= 0) {
		ssize_t n = read(peer->fd, buffer, sizeof(buffer));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			drop(peer);
			return;
		}
		for (ssize_t i = 0; i < n; i++) {
			struct bgpd_ipc_command command;

			if (buffer[i] != '\n') {
				if (peer->used + 1 >= sizeof(peer->line)) {
					drop(peer);
					return;
				}
				peer->line[peer->used++] = buffer[i];
				continue;
			}
			peer->line[peer->used] = '\0';
			peer->used = 0;
			if (bgpd_ipc_parse(peer->line, &command) == 0)
				handler(context, &command);
		}
	}
}

void bgpd_ipc_serve(struct bgpd_ipc_server *server, const struct bgpd_mgmt *m,
		    bgpd_ipc_handler handler, void *context)
{
	size_t size;

	if (server->listener < 0)
		return;
	accept_clients(server);
	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		read_commands(&server->peers[i], handler, context);
	size = bgpd_ipc_snapshot(m, server->work, sizeof(server->work));
	if (size == 0 || (size == server->last_size &&
			  memcmp(server->work, server->last, size) == 0))
		return;
	memcpy(server->last, server->work, size);
	server->last_size = size;
	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		if (server->peers[i].fd >= 0)
			send_snapshot(&server->peers[i], server->last, size);
}

size_t bgpd_ipc_clients(const struct bgpd_ipc_server *server)
{
	size_t n = 0;

	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		if (server->peers[i].fd >= 0)
			n++;
	return n;
}

void bgpd_ipc_server_close(struct bgpd_ipc_server *server)
{
	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		drop(&server->peers[i]);
	if (server->listener >= 0) {
		close(server->listener);
		unlink(server->path);
	}
	server->listener = -1;
}
