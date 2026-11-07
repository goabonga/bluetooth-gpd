/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "streams.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GREETING_TIMEOUT_MS 5000U
#define HEX_DIGITS 14U

static const char *const greetings[2] = {"BGPDPCM2", "BGPDCAP2"};
static const char *const names[2] = {"bluetooth-audio.sock",
				     "bluetooth-capture.sock"};

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

void bgpd_streams_init(struct bgpd_streams *s)
{
	memset(s, 0, sizeof(*s));
	s->listener[0] = s->listener[1] = -1;
	for (size_t i = 0; i < BGPD_STREAMS_PENDING; i++)
		s->pending[i].fd = -1;
}

int bgpd_streams_default_path(enum bgpd_stream_kind kind, char *buffer,
			      size_t size)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int length;

	if (dir == NULL || dir[0] != '/') {
		errno = ENOENT;
		return -1;
	}
	length = snprintf(buffer, size, "%s/%s", dir, names[kind]);
	if (length < 0 || (size_t)length >= size ||
	    (size_t)length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

int bgpd_streams_listen(struct bgpd_streams *s, enum bgpd_stream_kind kind,
			const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd, saved;

	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(address.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int probe;
		bool alive;

		if (errno != EADDRINUSE)
			goto fail;
		/* A socket left by a stopped daemon is replaced. */
		probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		alive =
		    probe >= 0 && connect(probe, (struct sockaddr *)&address,
					  sizeof(address)) == 0;
		if (probe >= 0)
			close(probe);
		if (alive) {
			errno = EADDRINUSE;
			goto fail;
		}
		unlink(path);
		if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
			goto fail;
	}
	if (chmod(path, S_IRUSR | S_IWUSR) < 0 || listen(fd, 4) < 0) {
		saved = errno;
		unlink(path);
		errno = saved;
		goto fail;
	}
	s->listener[kind] = fd;
	snprintf(s->path[kind], sizeof(s->path[kind]), "%s", path);
	return 0;
fail:
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}

bool bgpd_stream_accept(int fd, unsigned int rate)
{
	uint8_t bytes[4];

	for (unsigned int i = 0; i < 4; i++)
		bytes[i] = (uint8_t)(rate >> (8U * i));
	return send(fd, bytes, sizeof(bytes), MSG_NOSIGNAL | MSG_DONTWAIT) ==
	       (ssize_t)sizeof(bytes);
}

void bgpd_stream_refuse(int fd)
{
	bgpd_stream_accept(fd, 0);
	close(fd);
}

static void drop(struct bgpd_stream_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->used = 0;
}

/* The accepted sockets of one listener wait for their greeting. */
static void accept_clients(struct bgpd_streams *s, enum bgpd_stream_kind kind)
{
	for (;;) {
		struct ucred cred;
		socklen_t length = sizeof(cred);
		struct bgpd_stream_client *slot = NULL;
		int fd = accept4(s->listener[kind], NULL, NULL,
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
		for (size_t i = 0; i < BGPD_STREAMS_PENDING; i++)
			if (s->pending[i].fd < 0) {
				slot = &s->pending[i];
				break;
			}
		if (slot == NULL) {
			bgpd_stream_refuse(fd);
			continue;
		}
		slot->fd = fd;
		slot->kind = kind;
		slot->used = 0;
		slot->deadline = now_ms() + GREETING_TIMEOUT_MS;
	}
}

/* Reads the 14 hexadecimal digits of an address, true if they are valid. */
static bool read_address(const char *text, uint8_t addr[BGPD_ADDR_SIZE])
{
	for (size_t i = 0; i < BGPD_ADDR_SIZE; i++) {
		unsigned int value = 0;

		for (size_t j = 0; j < 2; j++) {
			char c = text[2 * i + j];

			value <<= 4U;
			if (c >= '0' && c <= '9')
				value |= (unsigned int)(c - '0');
			else if (c >= 'a' && c <= 'f')
				value |= (unsigned int)(c - 'a' + 10);
			else
				return false;
		}
		addr[i] = (uint8_t)value;
	}
	return true;
}

/* Parses a complete greeting; false drops the client. */
static bool complete(const struct bgpd_stream_client *c,
		     struct bgpd_stream_request *request)
{
	static const uint8_t zero[BGPD_ADDR_SIZE] = {0};

	if (memcmp(c->greeting, greetings[c->kind], 8) != 0 ||
	    c->greeting[BGPD_STREAMS_GREETING - 1] != '\n' ||
	    !read_address(c->greeting + 8, request->addr) ||
	    request->addr[6] > BGPD_ADDR_LE_RANDOM)
		return false;
	request->any = memcmp(request->addr, zero, BGPD_ADDR_SIZE) == 0;
	request->kind = c->kind;
	return true;
}

bool bgpd_streams_next(struct bgpd_streams *s,
		       struct bgpd_stream_request *request)
{
	for (int kind = 0; kind < 2; kind++)
		if (s->listener[kind] >= 0)
			accept_clients(s, (enum bgpd_stream_kind)kind);
	for (size_t i = 0; i < BGPD_STREAMS_PENDING; i++) {
		struct bgpd_stream_client *c = &s->pending[i];
		ssize_t n;

		if (c->fd < 0)
			continue;
		n = read(c->fd, c->greeting + c->used,
			 BGPD_STREAMS_GREETING - c->used);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR &&
			       errno != EWOULDBLOCK)) {
			drop(c);
			continue;
		}
		if (n > 0)
			c->used += (size_t)n;
		if (c->used == BGPD_STREAMS_GREETING) {
			if (complete(c, request)) {
				request->fd = c->fd;
				c->fd = -1;
				c->used = 0;
				return true;
			}
			bgpd_stream_refuse(c->fd);
			c->fd = -1;
			c->used = 0;
		} else if (now_ms() > c->deadline) {
			bgpd_stream_refuse(c->fd);
			c->fd = -1;
			c->used = 0;
		}
	}
	return false;
}

void bgpd_streams_shutdown(struct bgpd_streams *s)
{
	for (size_t i = 0; i < BGPD_STREAMS_PENDING; i++)
		drop(&s->pending[i]);
	for (int kind = 0; kind < 2; kind++) {
		if (s->listener[kind] >= 0) {
			close(s->listener[kind]);
			unlink(s->path[kind]);
		}
		s->listener[kind] = -1;
	}
}
