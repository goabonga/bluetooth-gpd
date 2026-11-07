/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * A capture client brings up the whole chain against a fake headset: SDP,
 * the hands-free AT dialogue, the mSBC codec negotiation and the voice link.
 * The microphone it receives is checked.
 */
#include "voice.h"

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
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
static const uint8_t lists[] = {0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04,
				0x35, 0x0c, 0x35, 0x03, 0x19, 0x01, 0x00,
				0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x07};

static struct bgpd_voice v;
static int sdp_end, sdp_peer, rfcomm_end, rfcomm_peer, sco_end, sco_peer;

static int open_sdp(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return sdp_end;
}

static int open_rfcomm(const uint8_t addr[BGPD_ADDR_SIZE], uint8_t channel)
{
	(void)addr;
	(void)channel;
	return rfcomm_end;
}

static int open_link(const uint8_t addr[BGPD_ADDR_SIZE],
		     enum bgpd_hfp_codec codec)
{
	(void)addr;
	(void)codec;
	return sco_end;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int connect_client(const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);

	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (fd >= 0 &&
	    connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void make_fakes(void)
{
	int fds[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	sdp_end = fds[0];
	sdp_peer = fds[1];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
	rfcomm_end = fds[0];
	rfcomm_peer = fds[1];
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	sco_end = fds[0];
	sco_peer = fds[1];
	bgpd_voice_init(&v);
	v.hfp.sdp.open_channel = open_sdp;
	v.hfp.open_channel = open_rfcomm;
	v.sco.open_link = open_link;
}

static void drop_fakes(void)
{
	close(sdp_peer);
	close(rfcomm_peer);
	close(sco_peer);
}

/* The headset side: answers the SDP request, speaks AT, sends voice. */
static void headset_side(sbc_t *encoder, int *step, size_t *frames_sent)
{
	uint8_t request[64], out[64], data[1024];
	ssize_t n;

	n = read(sdp_peer, request, sizeof(request));
	if (n >= 18) {
		out[0] = 0x07;
		out[1] = request[1];
		out[2] = request[2];
		out[3] = 0;
		out[4] = 2 + sizeof(lists) + 1;
		out[5] = 0;
		out[6] = sizeof(lists);
		memcpy(out + 7, lists, sizeof(lists));
		out[7 + sizeof(lists)] = 0;
		CHECK(write(sdp_peer, out, 8 + sizeof(lists)) > 0);
		*step = 1;
	}
	if (*step == 1) {
		static const char at[] = "AT+BRSF=190\rAT+BAC=1,2\rAT+CIND=?\r"
					 "AT+CIND?\rAT+CMER=3,0,0,1\r";

		/* Wait until the RFCOMM channel is being served. */
		if (write(rfcomm_peer, at, strlen(at)) > 0)
			*step = 2;
	}
	n = read(rfcomm_peer, data, sizeof(data) - 1);
	if (n > 0) {
		data[n] = '\0';
		if (strstr((char *)data, "+BCS: 2") != NULL) {
			CHECK(write(rfcomm_peer, "AT+BCS=2\r", 9) == 9);
			*step = 3;
		}
	}
	if (*step == 3 && *frames_sent < 40) {
		static const uint8_t headers[4] = {0x08, 0x38, 0xc8, 0xf8};
		int16_t wave[120];
		uint8_t packet[60];
		ssize_t written = 0;

		for (size_t i = 0; i < 120; i++)
			wave[i] =
			    (int16_t)(8000.0 *
				      sin(2.0 * 3.14159265358979 * 1000.0 *
					  (double)(*frames_sent * 120 + i) /
					  16000.0));
		packet[0] = 0x01;
		packet[1] = headers[*frames_sent % 4];
		CHECK(sbc_encode(encoder, wave, 240, packet + 2, 57,
				 &written) >= 0);
		packet[59] = 0;
		if (write(sco_peer, packet, 60) == 60)
			(*frames_sent)++;
	}
}

static void test_refused(const char *path)
{
	uint8_t rate[4];
	int client;

	make_fakes();
	CHECK(bgpd_voice_listen(&v, path) == 0);
	/* Without a headset in use the client is refused with rate 0. */
	client = connect_client(path);
	CHECK(client >= 0);
	CHECK(write(client, "BGPDCAP1", 8) == 8);
	for (int i = 0; i < 20; i++) {
		bgpd_voice_poll(&v);
		usleep(2000);
	}
	CHECK(read(client, rate, sizeof(rate)) == 4 && rate[0] == 0 &&
	      rate[1] == 0);
	CHECK(v.state == BGPD_VOICE_IDLE &&
	      strstr(v.status, "No headset") != NULL);
	close(client);

	/* A wrong greeting too. */
	bgpd_voice_target(&v, headset, "Headset");
	client = connect_client(path);
	CHECK(write(client, "NOTVALID", 8) == 8);
	for (int i = 0; i < 20; i++) {
		bgpd_voice_poll(&v);
		usleep(2000);
	}
	CHECK(read(client, rate, sizeof(rate)) == 4 && rate[0] == 0);
	close(client);
	bgpd_voice_shutdown(&v);
	drop_fakes();
}

static void test_microphone(const char *path)
{
	sbc_t encoder;
	uint8_t rate[4], buffer[8192];
	int16_t heard[8192];
	size_t samples = 0, bytes = 0, frames_sent = 0;
	int step = 0, client;
	uint64_t start;

	make_fakes();
	CHECK(bgpd_voice_listen(&v, path) == 0);
	CHECK(sbc_init_msbc(&encoder, 0) == 0);
	bgpd_voice_target(&v, headset, "Headset");
	client = connect_client(path);
	CHECK(client >= 0);
	CHECK(write(client, "BGPDCAP1", 8) == 8);

	start = now_ms();
	memset(rate, 0, sizeof(rate));
	{
		size_t got = 0;

		while (now_ms() - start < 4000 && samples < 3000) {
			ssize_t n;

			bgpd_voice_poll(&v);
			headset_side(&encoder, &step, &frames_sent);
			if (got < 4) {
				n = read(client, rate + got, 4 - got);
				if (n > 0)
					got += (size_t)n;
			} else {
				n = read(client, buffer + bytes % 2,
					 sizeof(buffer) - bytes % 2);
				if (n > 0) {
					size_t total = bytes % 2 + (size_t)n;

					for (size_t i = 0;
					     i + 1 < total && samples < 8192;
					     i += 2)
						heard[samples++] =
						    (int16_t)(buffer[i] |
							      (unsigned int)
								      buffer[i +
									     1]
								  << 8U);
					bytes = total % 2;
					if (bytes == 1)
						buffer[0] = buffer[total - 1];
				}
			}
			usleep(1000);
		}
	}
	/* mSBC: 16 kHz. The tone is 1 kHz: 2 crossings per period. */
	CHECK((unsigned int)rate[0] + 256U * rate[1] + 65536U * rate[2] ==
	      16000);
	CHECK(samples >= 1000);
	{
		unsigned int crossings = 0;
		size_t first = 240,
		       count = samples > first ? samples - first : 0;

		for (size_t i = first + 1; i < samples; i++)
			if ((heard[i] < 0) != (heard[i - 1] < 0))
				crossings++;
		/* One crossing every 8 samples: 1/8 of the count. */
		CHECK(count > 600 && crossings > count / 8 - 6 &&
		      crossings < count / 8 + 6);
	}
	CHECK(bgpd_voice_streaming(&v));
	CHECK(strstr(v.status, "microphone on") != NULL);

	/* When the client leaves, the voice and the control links close. */
	close(client);
	for (int i = 0; i < 50; i++) {
		bgpd_voice_poll(&v);
		usleep(2000);
	}
	CHECK(v.state == BGPD_VOICE_IDLE);
	CHECK(!bgpd_sco_active(&v.sco) && !bgpd_hfp_active(&v.hfp));

	sbc_finish(&encoder);
	bgpd_voice_shutdown(&v);
	drop_fakes();
}

int main(void)
{
	char dir[] = "/tmp/bgpd-voice-XXXXXX";
	char path[100];

	if (mkdtemp(dir) == NULL)
		return 2;
	snprintf(path, sizeof(path), "%s/capture.sock", dir);
	test_refused(path);
	test_microphone(path);
	rmdir(dir);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
