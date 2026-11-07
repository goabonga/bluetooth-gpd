/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "hfp.h"

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

static struct bgpd_hfp h;
static int peer;
static char answer[1024];

/* The headset sends an AT command; what the program answers is kept. */
static void send_at(const char *command)
{
	struct pollfd p = {.fd = peer, .events = POLLIN};
	size_t used = 0;

	CHECK(write(peer, command, strlen(command)) ==
	      (ssize_t)strlen(command));
	bgpd_hfp_poll(&h);
	while (used + 1 < sizeof(answer) && poll(&p, 1, 50) == 1) {
		ssize_t n =
		    read(peer, answer + used, sizeof(answer) - used - 1);

		if (n <= 0)
			break;
		used += (size_t)n;
	}
	answer[used] = '\0';
}

static void attach(void)
{
	int fds[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
	peer = fds[1];
	bgpd_hfp_init(&h);
	CHECK(bgpd_hfp_attach(&h, fds[0], "Headset") == 0);
}

static void test_wide_band(void)
{
	attach();
	CHECK(h.state == BGPD_HFP_SLC);
	/* Features: codec negotiation (bit 7) is advertised back (bit 9). */
	send_at("AT+BRSF=190\r");
	CHECK(strstr(answer, "+BRSF: 512") != NULL &&
	      strstr(answer, "OK") != NULL);
	send_at("AT+BAC=1,2\r");
	CHECK(strstr(answer, "OK") != NULL && h.msbc);
	send_at("AT+CIND=?\r");
	CHECK(strstr(answer, "\"service\"") != NULL &&
	      strstr(answer, "\"battchg\"") != NULL);
	send_at("AT+CIND?\r");
	CHECK(strstr(answer, "+CIND: 1,0,0,0,5,0,5") != NULL);
	CHECK(h.state == BGPD_HFP_SLC);
	send_at("AT+CMER=3,0,0,1\r");
	CHECK(strstr(answer, "OK") != NULL && h.state == BGPD_HFP_READY);
	CHECK(bgpd_hfp_codec(&h) == BGPD_HFP_MSBC);
	CHECK(strstr(h.status, "wide band") != NULL);

	/* The volume keys of the headset are followed. */
	send_at("AT+VGS=7\r");
	CHECK(h.speaker == 7 && h.gain_changed && strstr(answer, "OK") != NULL);
	send_at("AT+VGM=3\r");
	CHECK(h.microphone == 3);
	send_at("AT+VGS=99\r");
	CHECK(h.speaker == 7);

	/* The program sets them too. */
	CHECK(bgpd_hfp_set_speaker(&h, 9) == 0);
	CHECK(h.speaker == 9);
	CHECK(bgpd_hfp_set_microphone(&h, 40) == 0 && h.microphone == 15);
	send_at("");
	CHECK(strstr(answer, "+VGS: 9") != NULL &&
	      strstr(answer, "+VGM: 15") != NULL);

	/* The codec is announced, and the headset confirms it. */
	CHECK(bgpd_hfp_select_codec(&h) == 0);
	send_at("");
	CHECK(strstr(answer, "+BCS: 2") != NULL);
	send_at("AT+BCS=2\r");
	CHECK(h.codec_selected && strstr(answer, "OK") != NULL);
	send_at("AT+BCS=1\r");
	CHECK(!h.codec_selected && strstr(answer, "ERROR") != NULL);

	/* A request for the voice link gets the codec. */
	send_at("AT+BCC\r");
	CHECK(strstr(answer, "OK") != NULL &&
	      strstr(answer, "+BCS: 2") != NULL);

	/* Unknown commands are refused; the line may arrive in pieces. */
	send_at("AT+FOO\r");
	CHECK(strstr(answer, "ERROR") != NULL);
	send_at("AT+CL");
	CHECK(answer[0] == '\0');
	send_at("IP=1\r");
	CHECK(strstr(answer, "OK") != NULL);

	/* The headset goes away. */
	close(peer);
	bgpd_hfp_poll(&h);
	CHECK(!bgpd_hfp_active(&h) && strstr(h.status, "disconnected") != NULL);
}

static void test_narrow_band(void)
{
	attach();
	send_at("AT+BRSF=0\r");
	CHECK(strstr(answer, "+BRSF: 0") != NULL);
	send_at("AT+CMER=3,0,0,1\r");
	CHECK(h.state == BGPD_HFP_READY && bgpd_hfp_codec(&h) == BGPD_HFP_CVSD);
	CHECK(strstr(h.status, "narrow band") != NULL);
	/* Nothing to negotiate. */
	CHECK(bgpd_hfp_select_codec(&h) == 0 && h.codec_selected);
	send_at("");
	CHECK(answer[0] == '\0');
	/* A headset that lists mSBC without the feature still gets CVSD. */
	send_at("AT+BAC=1,2\r");
	CHECK(bgpd_hfp_codec(&h) == BGPD_HFP_CVSD);
	bgpd_hfp_close(&h);
	close(peer);
	CHECK(bgpd_hfp_set_speaker(&h, 3) == -1);
}

/* Fake SDP server and RFCOMM channel for the connection. */
static int sdp_end, rfcomm_end, sdp_peer, rfcomm_peer;
static uint8_t opened_channel;

static int open_sdp(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return sdp_end;
}

static int open_rfcomm(const uint8_t addr[BGPD_ADDR_SIZE], uint8_t channel)
{
	(void)addr;
	opened_channel = channel;
	return rfcomm_end;
}

static void test_connect(void)
{
	static const uint8_t lists[] = {
	    0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
	    0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x07};
	uint8_t request[64], out[64];
	int fds[2];
	struct pollfd p;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	sdp_end = fds[0];
	sdp_peer = fds[1];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
	rfcomm_end = fds[0];
	rfcomm_peer = fds[1];
	bgpd_hfp_init(&h);
	h.sdp.open_channel = open_sdp;
	h.open_channel = open_rfcomm;
	CHECK(bgpd_hfp_connect(&h, headset, "Headset") == 0);
	CHECK(h.state == BGPD_HFP_SDP);
	bgpd_hfp_poll(&h);
	p = (struct pollfd){.fd = sdp_peer, .events = POLLIN};
	CHECK(poll(&p, 1, 1000) == 1);
	CHECK(read(sdp_peer, request, sizeof(request)) >= 18);
	CHECK(request[7] == 0x19 && request[8] == 0x11 && request[9] == 0x1e);
	out[0] = 0x07;
	out[1] = request[1];
	out[2] = request[2];
	out[3] = 0;
	out[4] = 2 + sizeof(lists) + 1;
	out[5] = 0;
	out[6] = sizeof(lists);
	memcpy(out + 7, lists, sizeof(lists));
	out[7 + sizeof(lists)] = 0;
	CHECK(write(sdp_peer, out, 8 + sizeof(lists)) ==
	      (ssize_t)(8 + sizeof(lists)));
	bgpd_hfp_poll(&h);
	CHECK(opened_channel == 7);
	bgpd_hfp_poll(&h);
	CHECK(h.state == BGPD_HFP_SLC);
	peer = rfcomm_peer;
	send_at("AT+BRSF=0\r");
	CHECK(strstr(answer, "+BRSF: 0") != NULL);
	bgpd_hfp_close(&h);
	close(rfcomm_peer);
	close(sdp_peer);

	/* A device without the service fails the connection. */
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	sdp_end = fds[0];
	sdp_peer = fds[1];
	bgpd_hfp_init(&h);
	h.sdp.open_channel = open_sdp;
	CHECK(bgpd_hfp_connect(&h, headset, "Headset") == 0);
	bgpd_hfp_poll(&h);
	close(sdp_peer);
	bgpd_hfp_poll(&h);
	bgpd_hfp_poll(&h);
	CHECK(!bgpd_hfp_active(&h) &&
	      strstr(h.status, "no hands-free") != NULL);
}

int main(void)
{
	test_wide_band();
	test_narrow_band();
	test_connect();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
