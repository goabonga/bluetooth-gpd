/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "avrcp.h"

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

static struct bgpd_avrcp a;
static int peer, channel;
static uint8_t frame[256];
static ssize_t frame_size;

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
	bgpd_avrcp_init(&a);
	a.open_channel = open_channel;
}

/* The next command of the program, checked for its type and PDU. */
static void expect(uint8_t ctype, uint8_t pdu, size_t params)
{
	struct pollfd p = {.fd = peer, .events = POLLIN};

	if (poll(&p, 1, 1000) != 1) {
		CHECK(!"command expected");
		frame_size = 0;
		return;
	}
	frame_size = read(peer, frame, sizeof(frame));
	CHECK(frame_size == (ssize_t)(13 + params));
	CHECK((frame[0] & 3U) == 0 && frame[1] == 0x11 && frame[2] == 0x0e);
	CHECK(frame[3] == ctype && frame[4] == 0x48 && frame[5] == 0x00);
	CHECK(frame[6] == 0x00 && frame[7] == 0x19 && frame[8] == 0x58);
	CHECK(frame[9] == pdu && frame[10] == 0);
	CHECK(frame[12] == params);
}

/* A response of the device to the last command. */
static void respond(uint8_t ctype, uint8_t pdu, const uint8_t *params,
		    size_t length)
{
	uint8_t out[64] = {(uint8_t)((frame[0] & 0xf0U) | 2U),
			   0x11,
			   0x0e,
			   ctype,
			   0x48,
			   0x00,
			   0x00,
			   0x19,
			   0x58,
			   pdu,
			   0,
			   0,
			   (uint8_t)length};

	memcpy(out + 13, params, length);
	CHECK(write(peer, out, 13 + length) == (ssize_t)(13 + length));
	bgpd_avrcp_poll(&a);
}

static void test_volume(void)
{
	const uint8_t interim[] = {0x0d, 0x40};
	const uint8_t accepted[] = {32};
	const uint8_t changed[] = {0x0d, 0x7f};

	start();
	CHECK(bgpd_avrcp_connect(&a, headset) == 0);
	CHECK(a.state == BGPD_AVRCP_CONNECTING);
	bgpd_avrcp_poll(&a);
	CHECK(a.state == BGPD_AVRCP_ACTIVE);
	CHECK(bgpd_avrcp_set_volume(&a, 10) == -1); /* not known yet */

	/* The registration asks for the volume events. */
	expect(0x03, 0x31, 5);
	CHECK(frame[13] == 0x0d);
	respond(0x0f, 0x31, interim, sizeof(interim));
	CHECK(a.supported && a.volume == 50 && a.changed);
	a.changed = false;

	/* Setting the volume sends the device value, 25 % being 32/127. */
	CHECK(bgpd_avrcp_set_volume(&a, 25) == 0);
	expect(0x00, 0x50, 1);
	CHECK(frame[13] == 32);
	respond(0x09, 0x50, accepted, sizeof(accepted));
	CHECK(a.volume == 25 && a.changed);

	/* The buttons of the headset change it, and the program registers
	 * again. */
	respond(0x0d, 0x31, changed, sizeof(changed));
	CHECK(a.volume == 100);
	expect(0x03, 0x31, 5);

	/* The commands of the device are not implemented. */
	{
		const uint8_t command[] = {0x30, 0x11, 0x0e, 0x01, 0x48,
					   0x00, 0x00, 0x19, 0x58, 0x10,
					   0,	 0,    1,    0x03};

		CHECK(write(peer, command, sizeof(command)) ==
		      (ssize_t)sizeof(command));
		bgpd_avrcp_poll(&a);
		frame_size = read(peer, frame, sizeof(frame));
		CHECK(frame_size == (ssize_t)sizeof(command));
		CHECK((frame[0] & 0xf2U) == 0x32 && frame[3] == 0x08);
	}

	bgpd_avrcp_close(&a);
	CHECK(!bgpd_avrcp_active(&a) && a.volume == -1 && !a.supported);
	close(peer);
}

static void test_unsupported(void)
{
	start();
	CHECK(bgpd_avrcp_connect(&a, headset) == 0);
	bgpd_avrcp_poll(&a);
	expect(0x03, 0x31, 5);
	respond(0x08, 0x31, (const uint8_t[]){0x0d}, 1);
	CHECK(!a.supported && a.volume == -1);
	CHECK(bgpd_avrcp_set_volume(&a, 50) == -1);
	CHECK(a.state == BGPD_AVRCP_ACTIVE);
	bgpd_avrcp_close(&a);
	close(peer);
}

static void test_limits(void)
{
	int fds[2];

	start();
	{
		uint8_t le[BGPD_ADDR_SIZE] = {
		    1, 2, 3, 4, 5, 6, BGPD_ADDR_LE_PUBLIC};

		CHECK(bgpd_avrcp_connect(&a, le) == -1);
	}
	close(channel);
	close(peer);

	/* A connection that fails closes the state machine. */
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	channel = fds[0];
	close(fds[1]);
	CHECK(bgpd_avrcp_connect(&a, headset) == 0);
	bgpd_avrcp_poll(&a);
	bgpd_avrcp_poll(&a);
	CHECK(!bgpd_avrcp_active(&a));
}

int main(void)
{
	test_volume();
	test_unsupported();
	test_limits();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
