/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "sdp.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static const uint8_t headset[BGPD_ADDR_SIZE] = {
    1, 2, 3, 4, 5, 6, BGPD_ADDR_BREDR};

/* The attribute lists of a Hands-Free service on RFCOMM channel 7. */
static const uint8_t lists[] = {0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04,
				0x35, 0x0c, 0x35, 0x03, 0x19, 0x01, 0x00,
				0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x07};

static struct bgpd_sdp q;
static int peer, channel;
static uint8_t request[256];
static ssize_t request_size;

static int open_channel(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return channel;
}

static void start(void)
{
	int fds[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	channel = fds[0];
	peer = fds[1];
	bgpd_sdp_init(&q);
	q.open_channel = open_channel;
}

static uint16_t transaction(void)
{
	return request_size >= 3 ? (uint16_t)(256U * request[1] + request[2])
				 : 0;
}

static void expect_request(void)
{
	struct pollfd p = {.fd = peer, .events = POLLIN};

	if (poll(&p, 1, 1000) != 1) {
		CHECK(!"request expected");
		request_size = 0;
		return;
	}
	request_size = read(peer, request, sizeof(request));
	CHECK(request_size >= 18 && request[0] == 0x06);
}

/* Answers with part of the lists and a continuation state. */
static void respond(const uint8_t *part, size_t size, const uint8_t *cont,
		    size_t cont_size, uint16_t transaction)
{
	uint8_t out[512];
	size_t length = 2 + size + 1 + cont_size;

	out[0] = 0x07;
	out[1] = (uint8_t)(transaction >> 8U);
	out[2] = (uint8_t)transaction;
	out[3] = (uint8_t)(length >> 8U);
	out[4] = (uint8_t)length;
	out[5] = (uint8_t)(size >> 8U);
	out[6] = (uint8_t)size;
	memcpy(out + 7, part, size);
	out[7 + size] = (uint8_t)cont_size;
	if (cont_size > 0)
		memcpy(out + 8 + size, cont, cont_size);
	CHECK(write(peer, out, 8 + size + cont_size) ==
	      (ssize_t)(8 + size + cont_size));
}

static void test_parse(void)
{
	uint8_t found = 0;
	/* The same with the 128-bit form of the RFCOMM UUID. */
	static const uint8_t long_form[] = {0x35, 0x15, 0x35, 0x13, 0x1c, 0x00,
					    0x00, 0x00, 0x03, 0x00, 0x00, 0x10,
					    0x00, 0x80, 0x00, 0x00, 0x80, 0x5f,
					    0x9b, 0x34, 0xfb, 0x08, 0x05};
	static const uint8_t other[] = {0x35, 0x08, 0x35, 0x06, 0x19,
					0x01, 0x00, 0x08, 0x07, 0x00};
	static const uint8_t no_channel[] = {0x35, 0x05, 0x19,
					     0x00, 0x03, 0x00};

	CHECK(bgpd_sdp_parse(lists, sizeof(lists), &found) == 0 && found == 7);
	found = 0;
	CHECK(bgpd_sdp_parse(long_form, sizeof(long_form), &found) == 0);
	CHECK(found == 5);
	/* Nothing to find, or malformed. */
	CHECK(bgpd_sdp_parse(other, sizeof(other), &found) == -1);
	CHECK(bgpd_sdp_parse(no_channel, sizeof(no_channel), &found) == -1);
	CHECK(bgpd_sdp_parse(lists, sizeof(lists) - 3, &found) == -1);
	CHECK(bgpd_sdp_parse(lists, 0, &found) == -1);
	/* A deeply nested sequence is refused. */
	{
		uint8_t deep[40];

		for (size_t i = 0; i < 20; i++) {
			deep[2 * i] = 0x35;
			deep[2 * i + 1] = (uint8_t)(38 - 2 * i);
		}
		memset(deep + 40 - 2, 0, 2);
		CHECK(bgpd_sdp_parse(deep, sizeof(deep), &found) == -1);
	}
}

static void test_query(void)
{
	start();
	CHECK(bgpd_sdp_start(&q, headset, 0x111e) == 0);
	CHECK(!bgpd_sdp_poll(&q) || q.state == BGPD_SDP_FAILED);
	expect_request();
	/* The pattern holds the UUID of the service; no continuation yet. */
	CHECK(memcmp(request + 5, (uint8_t[]){0x35, 3, 0x19, 0x11, 0x1e}, 5) ==
	      0);
	CHECK(request_size >= 18);
	if (request_size >= 18)
		CHECK(request[request_size - 1] == 0);
	respond(lists, sizeof(lists), NULL, 0, transaction());
	CHECK(bgpd_sdp_poll(&q));
	CHECK(q.state == BGPD_SDP_FOUND && q.channel == 7);
	close(peer);
}

static void test_continuation(void)
{
	const uint8_t cont[2] = {0xaa, 0xbb};

	start();
	CHECK(bgpd_sdp_start(&q, headset, 0x111e) == 0);
	bgpd_sdp_poll(&q);
	expect_request();
	respond(lists, 10, cont, sizeof(cont), transaction());
	CHECK(!bgpd_sdp_poll(&q));
	/* The next request carries the continuation state. */
	expect_request();
	CHECK(request_size >= 18);
	if (request_size >= 18) {
		const uint8_t *tail = request + request_size - 3;

		CHECK(tail[0] == 2 && tail[1] == 0xaa && tail[2] == 0xbb);
	}
	respond(lists + 10, sizeof(lists) - 10, NULL, 0, transaction());
	CHECK(bgpd_sdp_poll(&q));
	CHECK(q.state == BGPD_SDP_FOUND && q.channel == 7);
	close(peer);
}

static void test_failures(void)
{
	static const uint8_t empty[] = {0x35, 0x00};

	/* Answer to another transaction. */
	start();
	CHECK(bgpd_sdp_start(&q, headset, 0x111e) == 0);
	bgpd_sdp_poll(&q);
	expect_request();
	respond(lists, sizeof(lists), NULL, 0, 0x7777);
	CHECK(bgpd_sdp_poll(&q) && q.state == BGPD_SDP_FAILED);
	close(peer);

	/* No service. */
	start();
	CHECK(bgpd_sdp_start(&q, headset, 0x111e) == 0);
	bgpd_sdp_poll(&q);
	expect_request();
	respond(empty, sizeof(empty), NULL, 0, transaction());
	CHECK(bgpd_sdp_poll(&q) && q.state == BGPD_SDP_FAILED);
	close(peer);

	/* The device hangs up. */
	start();
	CHECK(bgpd_sdp_start(&q, headset, 0x111e) == 0);
	bgpd_sdp_poll(&q);
	expect_request();
	close(peer);
	CHECK(bgpd_sdp_poll(&q) && q.state == BGPD_SDP_FAILED);

	/* Only Classic devices have an SDP server we can ask. */
	{
		uint8_t le[BGPD_ADDR_SIZE] = {
		    1, 2, 3, 4, 5, 6, BGPD_ADDR_LE_PUBLIC};

		bgpd_sdp_init(&q);
		CHECK(bgpd_sdp_start(&q, le, 0x111e) == -1);
	}
}

int main(void)
{
	test_parse();
	test_query();
	test_continuation();
	test_failures();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
