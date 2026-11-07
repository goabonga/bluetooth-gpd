/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "avdtp.h"

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

static struct bgpd_avdtp a;
static int sink_sig, sink_media; /* the sink's ends of the channels */
static int channels[2], next_channel;
static uint8_t pdu[1024];
static ssize_t pdu_size;

/* The "Bluetooth" channels of the test are connected socket pairs. */
static int open_channel(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return channels[next_channel++];
}

static void connect_pairs(void)
{
	int sig[2], media[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sig) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, media) ==
	      0);
	channels[0] = sig[0];
	channels[1] = media[0];
	sink_sig = sig[1];
	sink_media = media[1];
	next_channel = 0;
}

static void drop_pairs(void)
{
	bgpd_avdtp_close(&a);
	close(sink_sig);
	close(sink_media);
}

/* The next signaling command of the program, checked for its signal. */
static void expect(uint8_t signal, const uint8_t *payload, size_t size)
{
	struct pollfd p = {.fd = sink_sig, .events = POLLIN};

	if (poll(&p, 1, 1000) != 1) {
		CHECK(!"command expected");
		pdu_size = 0;
		return;
	}
	pdu_size = read(sink_sig, pdu, sizeof(pdu));
	CHECK(pdu_size >= 2 && (pdu[0] & 3U) == 0 && pdu[1] == signal);
	CHECK(pdu_size == (ssize_t)(2 + size));
	if (pdu_size == (ssize_t)(2 + size) && size > 0)
		CHECK(memcmp(pdu + 2, payload, size) == 0);
}

static void answer(uint8_t type, const uint8_t *payload, size_t size)
{
	uint8_t out[512];

	out[0] = (uint8_t)((pdu[0] & 0xf0U) | type);
	out[1] = pdu[1];
	if (size > 0)
		memcpy(out + 2, payload, size);
	CHECK(write(sink_sig, out, size + 2) == (ssize_t)(size + 2));
	bgpd_avdtp_poll(&a);
}

static const uint8_t endpoints[] = {
    0x04, 0x00, /* 1: source, ignored */
    0x08, 0x08, /* 2: free audio sink */
    0x0e, 0x08, /* 3: sink already in use, ignored */
};

static void negotiate(const uint8_t *caps, size_t size)
{
	const uint8_t seid2 = 0x08;

	CHECK(bgpd_avdtp_connect(&a, headset, "Headset") == 0);
	bgpd_avdtp_poll(&a);
	expect(0x01, NULL, 0);
	answer(2, endpoints, sizeof(endpoints));
	expect(0x02, &seid2, 1);
	answer(2, caps, size);
}

static void test_stream(void)
{
	static const uint8_t caps[] = {1, 0, 7, 6, 0, 0, 0x3f, 0xff, 2, 64};
	static const uint8_t config[] = {0x08, 0x04, 1,	   0,	 7,  6,
					 0,    0,    0x21, 0x15, 53, 53};
	const uint8_t seid2 = 0x08;
	uint8_t frames[40];
	uint8_t packet[128];
	ssize_t n;

	connect_pairs();
	a.open_channel = open_channel;
	negotiate(caps, sizeof(caps));
	CHECK(a.state == BGPD_AVDTP_CONFIGURE);
	expect(0x03, config, sizeof(config));
	CHECK(a.sbc.rate == 44100 && a.sbc.channel_mode == 1 &&
	      a.sbc.bitpool == 53 && a.sbc.loudness);
	answer(2, NULL, 0);
	expect(0x06, &seid2, 1);
	answer(2, NULL, 0);
	CHECK(a.state == BGPD_AVDTP_MEDIA);
	bgpd_avdtp_poll(&a);
	CHECK(a.state == BGPD_AVDTP_READY);
	CHECK(strstr(a.status, "Audio ready") != NULL);
	CHECK(bgpd_avdtp_active(&a));

	/* Nothing is sent before the stream starts. */
	memset(frames, 0xa5, sizeof(frames));
	CHECK(bgpd_avdtp_send(&a, frames, sizeof(frames), 2, 256) == -1);
	CHECK(bgpd_avdtp_start(&a) == 0);
	expect(0x07, &seid2, 1);
	answer(2, NULL, 0);
	CHECK(a.state == BGPD_AVDTP_STREAMING);

	CHECK(bgpd_avdtp_send(&a, frames, sizeof(frames), 2, 256) == 0);
	n = read(sink_media, packet, sizeof(packet));
	CHECK(n == 12 + 1 + 40);
	CHECK(packet[0] == 0x80 && packet[1] == 0x60);
	CHECK(packet[2] == 0 && packet[3] == 0);
	CHECK(packet[4] == 0 && packet[7] == 0);
	CHECK(packet[12] == 2 && packet[13] == 0xa5);
	CHECK(bgpd_avdtp_send(&a, frames, sizeof(frames), 2, 256) == 0);
	n = read(sink_media, packet, sizeof(packet));
	CHECK(n == 12 + 1 + 40);
	CHECK(packet[3] == 1 && packet[6] == 1 && packet[7] == 0);
	CHECK(memcmp(packet + 8,
		     (uint8_t[4]){a.ssrc >> 24U, a.ssrc >> 16U, a.ssrc >> 8U,
				  a.ssrc},
		     4) == 0);

	/* Packets must fit the channel. */
	CHECK(bgpd_avdtp_capacity(&a) == 672 - 13);
	CHECK(bgpd_avdtp_send(&a, frames, 700, 2, 256) == -1);
	CHECK(bgpd_avdtp_send(&a, frames, 10, 0, 256) == -1);

	CHECK(bgpd_avdtp_suspend(&a) == 0);
	expect(0x09, &seid2, 1);
	answer(2, NULL, 0);
	CHECK(a.state == BGPD_AVDTP_READY);
	CHECK(bgpd_avdtp_suspend(&a) == -1);

	/* The sink closes the stream. */
	{
		const uint8_t close_command[2] = {0x50, 0x08};

		CHECK(write(sink_sig, close_command, 2) == 2);
		bgpd_avdtp_poll(&a);
		n = read(sink_sig, packet, sizeof(packet));
		CHECK(n == 2 && packet[0] == 0x52 && packet[1] == 0x08);
		CHECK(a.state == BGPD_AVDTP_IDLE && a.sig == -1);
	}
	drop_pairs();
}

static void test_no_endpoint(void)
{
	static const uint8_t sources[] = {0x04, 0x00};

	connect_pairs();
	a.open_channel = open_channel;
	CHECK(bgpd_avdtp_connect(&a, headset, "Headset") == 0);
	bgpd_avdtp_poll(&a);
	expect(0x01, NULL, 0);
	answer(2, sources, sizeof(sources));
	CHECK(a.state == BGPD_AVDTP_IDLE);
	CHECK(strstr(a.status, "no usable") != NULL);
	drop_pairs();
}

static void test_unusable_codec(void)
{
	/* 48 kHz mono only: no channel mode we can send. */
	static const uint8_t caps[] = {1, 0, 7, 6, 0, 0, 0x18, 0xff, 2, 64};

	connect_pairs();
	a.open_channel = open_channel;
	negotiate(caps, sizeof(caps));
	CHECK(a.state == BGPD_AVDTP_IDLE);
	CHECK(strstr(a.status, "no usable") != NULL);
	drop_pairs();
}

static void test_rejection(void)
{
	const uint8_t error = 0x31;

	connect_pairs();
	a.open_channel = open_channel;
	CHECK(bgpd_avdtp_connect(&a, headset, "Headset") == 0);
	bgpd_avdtp_poll(&a);
	expect(0x01, NULL, 0);
	answer(3, &error, 1);
	CHECK(a.state == BGPD_AVDTP_IDLE);
	CHECK(strstr(a.status, "refused") != NULL);
	drop_pairs();
}

static void test_limits(void)
{
	bgpd_avdtp_init(&a);
	CHECK(bgpd_avdtp_start(&a) == -1);
	CHECK(!bgpd_avdtp_active(&a));
	/* Only Classic devices stream audio. */
	{
		uint8_t le[BGPD_ADDR_SIZE] = {
		    1, 2, 3, 4, 5, 6, BGPD_ADDR_LE_PUBLIC};

		CHECK(bgpd_avdtp_connect(&a, le, "Keyboard") == -1);
	}
	/* A connection that fails is reported. */
	a.open_channel = open_channel;
	{
		int fds[2];

		CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);
		channels[0] = fds[0];
		next_channel = 0;
		close(fds[1]);
		CHECK(bgpd_avdtp_connect(&a, headset, "Headset") == 0);
		bgpd_avdtp_poll(&a);
		bgpd_avdtp_poll(&a);
		CHECK(a.state == BGPD_AVDTP_IDLE);
	}
}

int main(void)
{
	bgpd_avdtp_init(&a);
	test_stream();
	bgpd_avdtp_init(&a);
	test_no_endpoint();
	bgpd_avdtp_init(&a);
	test_unusable_codec();
	bgpd_avdtp_init(&a);
	test_rejection();
	test_limits();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
