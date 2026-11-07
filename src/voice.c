/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "voice.h"

#include "streams.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SETUP_TIMEOUT_MS 25000U
#define CHUNK_SAMPLES 2048U

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

void bgpd_voice_init(struct bgpd_voice *v)
{
	memset(v, 0, sizeof(*v));
	v->client = -1;
	bgpd_hfp_init(&v->hfp);
	bgpd_sco_init(&v->sco);
}

void bgpd_voice_target(struct bgpd_voice *v, const uint8_t addr[BGPD_ADDR_SIZE],
		       const char *name)
{
	v->has_target = true;
	memcpy(v->target, addr, BGPD_ADDR_SIZE);
	snprintf(v->target_name, sizeof(v->target_name), "%s", name);
}

void bgpd_voice_no_target(struct bgpd_voice *v)
{
	v->has_target = false;
}

int bgpd_voice_attach(struct bgpd_voice *v, int fd)
{
	if (v->state != BGPD_VOICE_IDLE || v->client >= 0 || !v->has_target ||
	    bgpd_hfp_connect(&v->hfp, v->target, v->target_name) < 0) {
		bgpd_stream_refuse(fd);
		return -1;
	}
	v->client = fd;
	v->deadline = now_ms() + SETUP_TIMEOUT_MS;
	v->state = BGPD_VOICE_HANDSFREE;
	snprintf(v->status, sizeof(v->status),
		 "Starting the headset microphone...");
	v->changed = true;
	return 0;
}

bool bgpd_voice_streaming(const struct bgpd_voice *v)
{
	return v->state == BGPD_VOICE_STREAMING;
}

static void say(struct bgpd_voice *v, const char *text)
{
	snprintf(v->status, sizeof(v->status), "%s", text);
	v->changed = true;
}

static void send_rate(int fd, unsigned int rate)
{
	uint8_t bytes[4];
	ssize_t n;

	for (unsigned int i = 0; i < 4; i++)
		bytes[i] = (uint8_t)(rate >> (8U * i));
	n = send(fd, bytes, sizeof(bytes), MSG_NOSIGNAL | MSG_DONTWAIT);
	(void)n;
}

/* Ends the client (told the rate 0 if the link never came up) and the links. */
static void finish(struct bgpd_voice *v, bool refused)
{
	if (v->client >= 0) {
		if (refused)
			send_rate(v->client, 0);
		close(v->client);
	}
	v->client = -1;
	bgpd_sco_close(&v->sco);
	bgpd_hfp_close(&v->hfp);
	v->state = BGPD_VOICE_IDLE;
}

/* Sends the microphone to the client; false when the client has left. */
static bool pump(struct bgpd_voice *v)
{
	int16_t samples[CHUNK_SAMPLES];
	char probe;
	size_t n;

	/* An end of file on the socket means the client left. */
	if (recv(v->client, &probe, 1, MSG_PEEK | MSG_DONTWAIT) == 0)
		return false;
	while ((n = bgpd_sco_read(&v->sco, samples, CHUNK_SAMPLES)) > 0) {
		ssize_t sent = send(v->client, samples, n * sizeof(samples[0]),
				    MSG_NOSIGNAL | MSG_DONTWAIT);

		if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return false;
		/* A client that is too slow loses audio instead of blocking. */
	}
	return true;
}

void bgpd_voice_poll(struct bgpd_voice *v)
{
	if (v->state == BGPD_VOICE_IDLE)
		return;
	bgpd_hfp_poll(&v->hfp);
	if (v->state == BGPD_VOICE_HANDSFREE) {
		if (!bgpd_hfp_active(&v->hfp)) {
			say(v, "The headset has no hands-free link");
			finish(v, true);
			return;
		}
		if (v->hfp.state == BGPD_HFP_READY) {
			bgpd_hfp_select_codec(&v->hfp);
			v->state = BGPD_VOICE_CODEC;
		}
	}
	if (v->state == BGPD_VOICE_CODEC && v->hfp.codec_selected) {
		if (bgpd_sco_open(&v->sco, v->target, bgpd_hfp_codec(&v->hfp)) <
		    0) {
			say(v, "Cannot open the voice link");
			finish(v, true);
			return;
		}
		v->state = BGPD_VOICE_LINK;
	}
	if (v->state == BGPD_VOICE_LINK || v->state == BGPD_VOICE_STREAMING) {
		bool alive = bgpd_sco_poll(&v->sco);

		if (!alive || !bgpd_hfp_active(&v->hfp)) {
			say(v, "The voice link ended");
			finish(v, v->state == BGPD_VOICE_LINK);
			return;
		}
		if (v->state == BGPD_VOICE_LINK && v->sco.opened) {
			send_rate(v->client, v->sco.rate);
			v->state = BGPD_VOICE_STREAMING;
			say(v, "Headset microphone on");
		}
	}
	if (v->state == BGPD_VOICE_STREAMING && !pump(v)) {
		say(v, "Headset microphone off");
		finish(v, false);
		return;
	}
	if (v->state != BGPD_VOICE_IDLE && v->state != BGPD_VOICE_STREAMING &&
	    now_ms() > v->deadline) {
		say(v, "The headset did not start its microphone");
		finish(v, true);
	}
}

void bgpd_voice_shutdown(struct bgpd_voice *v)
{
	finish(v, false);
}
