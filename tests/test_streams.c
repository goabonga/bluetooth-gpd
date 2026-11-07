/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "streams.h"

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

static int connect_to(const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (fd < 0 ||
	    connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		exit(2); /* the test cannot go on */
	return fd;
}

/* Calls next until a request comes, or a few times. */
static bool wait_request(struct bgpd_streams *s,
			 struct bgpd_stream_request *request)
{
	for (int i = 0; i < 50; i++) {
		if (bgpd_streams_next(s, request))
			return true;
		usleep(2000);
	}
	return false;
}

/* True if the client was refused with the rate 0. */
static bool refused(int fd)
{
	uint8_t rate[4];

	for (int i = 0; i < 50; i++) {
		ssize_t n = recv(fd, rate, sizeof(rate), MSG_DONTWAIT);

		if (n == 4)
			return rate[0] == 0 && rate[1] == 0 && rate[2] == 0 &&
			       rate[3] == 0;
		usleep(2000);
	}
	return false;
}

int main(void)
{
	struct bgpd_streams s;
	struct bgpd_stream_request request;
	char dir[] = "/tmp/bgpd-streams-XXXXXX";
	char playback[100], capture[100];
	struct stat st;
	int client, other;
	uint8_t rate[4];

	if (mkdtemp(dir) == NULL)
		return 2;
	snprintf(playback, sizeof(playback), "%s/audio.sock", dir);
	snprintf(capture, sizeof(capture), "%s/capture.sock", dir);
	bgpd_streams_init(&s);
	CHECK(bgpd_streams_listen(&s, BGPD_STREAM_PLAYBACK, playback) == 0);
	CHECK(bgpd_streams_listen(&s, BGPD_STREAM_CAPTURE, capture) == 0);
	CHECK(stat(playback, &st) == 0 && (st.st_mode & 0777U) == 0600U);
	CHECK(!bgpd_streams_next(&s, &request));

	/* A playback client names its device. */
	client = connect_to(playback);
	CHECK(write(client,
		    "BGPDPCM2"
		    "01020304050600\n",
		    23) == 23);
	CHECK(wait_request(&s, &request));
	CHECK(request.kind == BGPD_STREAM_PLAYBACK && !request.any);
	CHECK(request.addr[0] == 1 && request.addr[5] == 6 &&
	      request.addr[6] == 0);
	CHECK(bgpd_stream_accept(request.fd, 44100));
	CHECK(read(client, rate, sizeof(rate)) == 4);
	CHECK((unsigned int)rate[0] + 256U * rate[1] + 65536U * rate[2] ==
	      44100);
	close(request.fd);
	close(client);

	/* All zeros means the device last connected. */
	client = connect_to(capture);
	CHECK(write(client,
		    "BGPDCAP2"
		    "00000000000000\n",
		    23) == 23);
	CHECK(wait_request(&s, &request));
	CHECK(request.kind == BGPD_STREAM_CAPTURE && request.any);
	bgpd_stream_refuse(request.fd);
	CHECK(refused(client));
	close(client);

	/* A greeting that comes in pieces. */
	client = connect_to(playback);
	CHECK(write(client,
		    "BGPDPCM2"
		    "0a0b",
		    12) == 12);
	CHECK(!bgpd_streams_next(&s, &request));
	CHECK(write(client, "0c0d0e0f01\n", 11) == 11);
	CHECK(wait_request(&s, &request));
	CHECK(request.addr[0] == 0x0a && request.addr[5] == 0x0f &&
	      request.addr[6] == 1);
	close(request.fd);
	close(client);

	/* The greeting of the other kind, bad digits or type: refused. */
	client = connect_to(playback);
	other = connect_to(capture);
	CHECK(write(client,
		    "BGPDCAP2"
		    "01020304050600\n",
		    23) == 23);
	CHECK(write(other,
		    "BGPDCAP2"
		    "0102030405060g\n",
		    23) == 23);
	CHECK(!wait_request(&s, &request));
	CHECK(refused(client));
	CHECK(refused(other));
	close(client);
	close(other);
	client = connect_to(playback);
	CHECK(write(client,
		    "BGPDPCM2"
		    "01020304050609\n",
		    23) == 23);
	CHECK(!wait_request(&s, &request));
	CHECK(refused(client));
	close(client);

	/* A client that leaves before greeting is forgotten. */
	client = connect_to(playback);
	close(client);
	CHECK(!wait_request(&s, &request));

	bgpd_streams_shutdown(&s);
	CHECK(stat(playback, &st) == -1 && stat(capture, &st) == -1);
	rmdir(dir);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
