/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "sco.h"

#include <math.h>
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

static struct bgpd_sco s;
static int peer, link_end;
static enum bgpd_hfp_codec opened_codec;

static int open_link(const uint8_t addr[BGPD_ADDR_SIZE],
		     enum bgpd_hfp_codec codec)
{
	(void)addr;
	opened_codec = codec;
	return link_end;
}

static void start(enum bgpd_hfp_codec codec)
{
	int fds[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	link_end = fds[0];
	peer = fds[1];
	bgpd_sco_init(&s);
	s.open_link = open_link;
	CHECK(bgpd_sco_open(&s, headset, codec) == 0);
	CHECK(opened_codec == codec && bgpd_sco_active(&s));
	CHECK(bgpd_sco_poll(&s) && s.opened);
}

/* A sine wave of the given frequency, as 16-bit samples. */
static void sine(int16_t *samples, size_t count, size_t first,
		 unsigned int rate, double hz)
{
	for (size_t i = 0; i < count; i++)
		samples[i] =
		    (int16_t)(8000.0 * sin(2.0 * 3.14159265358979 * hz *
					   (double)(first + i) / rate));
}

static unsigned int crossings(const int16_t *samples, size_t count)
{
	unsigned int n = 0;

	for (size_t i = 1; i < count; i++)
		if ((samples[i] < 0) != (samples[i - 1] < 0))
			n++;
	return n;
}

static void test_cvsd(void)
{
	int16_t wave[480], heard[512], out[100];
	uint8_t packet[64];
	size_t total = 0;

	start(BGPD_HFP_CVSD);
	CHECK(s.rate == 8000);
	/* 480 samples of a 1 kHz tone arrive in packets of 48 bytes. */
	sine(wave, 480, 0, 8000, 1000.0);
	for (size_t i = 0; i < 480; i += 24) {
		CHECK(write(peer, wave + i, 48) == 48);
	}
	CHECK(bgpd_sco_poll(&s));
	total = bgpd_sco_read(&s, heard, 512);
	CHECK(total == 480);
	CHECK(memcmp(heard, wave, sizeof(wave)) == 0);
	CHECK(bgpd_sco_read(&s, heard, 512) == 0);

	/* The earpiece gets the samples in packets of the size of the link. */
	for (size_t i = 0; i < 100; i++)
		out[i] = (int16_t)(i * 10);
	CHECK(bgpd_sco_write(&s, out, 100) == 100);
	CHECK(bgpd_sco_poll(&s));
	for (size_t sent = 0; sent < 4; sent++) {
		CHECK(read(peer, packet, sizeof(packet)) == 48);
		CHECK(memcmp(packet, out + 24 * sent, 48) == 0);
	}
	CHECK(read(peer, packet, sizeof(packet)) == -1);

	/* The headset hangs up. */
	close(peer);
	CHECK(!bgpd_sco_poll(&s) && !bgpd_sco_active(&s));
}

static void test_msbc(void)
{
	sbc_t encoder, decoder;
	int16_t wave[120 * 6], heard[2048], out[120 * 5];
	uint8_t packet[64], frame[64];
	static const uint8_t headers[4] = {0x08, 0x38, 0xc8, 0xf8};
	size_t total;

	start(BGPD_HFP_MSBC);
	CHECK(s.rate == 16000);
	CHECK(sbc_init_msbc(&encoder, 0) == 0);
	CHECK(sbc_init_msbc(&decoder, 0) == 0);

	/* Six frames of a 1 kHz tone arrive, one cut in two packets. */
	sine(wave, 720, 0, 16000, 1000.0);
	for (size_t f = 0; f < 6; f++) {
		ssize_t written = 0;

		packet[0] = 0x01;
		packet[1] = headers[f % 4];
		CHECK(sbc_encode(&encoder, wave + 120 * f, 240, packet + 2, 57,
				 &written) >= 0 &&
		      written == 57);
		packet[59] = 0;
		if (f == 2) {
			CHECK(write(peer, packet, 24) == 24);
			CHECK(bgpd_sco_poll(&s));
			CHECK(write(peer, packet + 24, 36) == 36);
		} else {
			CHECK(write(peer, packet, 60) == 60);
		}
	}
	CHECK(bgpd_sco_poll(&s));
	total = bgpd_sco_read(&s, heard, 2048);
	CHECK(total == 720);
	/* A 1 kHz tone at 16 kHz crosses zero 2000 times a second. */
	CHECK(crossings(heard + 120, 600) > 70 &&
	      crossings(heard + 120, 600) < 80);

	/* Samples for the earpiece become packets with the H2 headers. */
	sine(out, 600, 0, 16000, 500.0);
	CHECK(bgpd_sco_write(&s, out, 600) == 600);
	CHECK(bgpd_sco_poll(&s));
	for (size_t f = 0; f < 5; f++) {
		size_t used = 0;
		int16_t pcm[120];
		size_t written = 0;

		CHECK(read(peer, packet, sizeof(packet)) == 60);
		CHECK(packet[0] == 0x01 && packet[1] == headers[f % 4]);
		CHECK(packet[2] == 0xad);
		memcpy(frame, packet + 2, 57);
		used = (size_t)sbc_decode(&decoder, frame, 57, pcm, sizeof(pcm),
					  &written);
		CHECK(used == 57 && written == 240);
		/* 500 Hz crosses zero 7 or 8 times in 7.5 ms; the first frame
		 * holds the start of the codec. */
		if (f > 0)
			CHECK(crossings(pcm, 120) >= 6 &&
			      crossings(pcm, 120) <= 9);
	}

	/* Noise that is not a frame is skipped. */
	memset(packet, 0x55, 60);
	CHECK(write(peer, packet, 60) == 60);
	CHECK(bgpd_sco_poll(&s));
	CHECK(bgpd_sco_read(&s, heard, 2048) == 0);

	sbc_finish(&encoder);
	sbc_finish(&decoder);
	bgpd_sco_close(&s);
	close(peer);
	CHECK(!bgpd_sco_active(&s));
}

static void test_limits(void)
{
	uint8_t le[BGPD_ADDR_SIZE] = {1, 2, 3, 4, 5, 6, BGPD_ADDR_LE_PUBLIC};
	int16_t samples[4] = {1, 2, 3, 4};

	bgpd_sco_init(&s);
	CHECK(bgpd_sco_open(&s, le, BGPD_HFP_CVSD) == -1);
	CHECK(!bgpd_sco_poll(&s));
	/* The rings take what fits. */
	CHECK(bgpd_sco_write(&s, samples, 4) == 4);
	CHECK(bgpd_sco_read(&s, samples, 4) == 0);
}

int main(void)
{
	test_cvsd();
	test_msbc();
	test_limits();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
