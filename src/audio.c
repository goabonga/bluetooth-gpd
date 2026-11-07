/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "audio.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define TONE_HZ 440.0
#define TONE_AMPLITUDE 6000.0
#define PI 3.14159265358979323846
/* Audio is sent this far ahead of real time, to absorb the polling delay. */
#define LEAD_MS 60U
#define MAX_FRAMES_PER_PACKET 15U
#define CHANNELS 2U

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

void bgpd_audio_init(struct bgpd_audio *audio)
{
	memset(audio, 0, sizeof(*audio));
	bgpd_avdtp_init(&audio->avdtp);
}

bool bgpd_audio_active(const struct bgpd_audio *audio)
{
	return bgpd_avdtp_active(&audio->avdtp);
}

bool bgpd_audio_streaming(const struct bgpd_audio *audio)
{
	return audio->avdtp.state == BGPD_AVDTP_STREAMING;
}

static void finish_encoder(struct bgpd_audio *audio)
{
	if (audio->encoding)
		sbc_finish(&audio->sbc);
	audio->encoding = false;
}

void bgpd_audio_close(struct bgpd_audio *audio)
{
	finish_encoder(audio);
	bgpd_avdtp_close(&audio->avdtp);
	audio->tone_ms = audio->tone_left = 0;
}

int bgpd_audio_connect(struct bgpd_audio *audio,
		       const uint8_t addr[BGPD_ADDR_SIZE], const char *name)
{
	return bgpd_avdtp_connect(&audio->avdtp, addr, name);
}

int bgpd_audio_tone(struct bgpd_audio *audio, unsigned int milliseconds)
{
	if (!bgpd_avdtp_active(&audio->avdtp))
		return -1;
	/* The rate is known once the sink is configured. */
	audio->tone_ms = milliseconds;
	audio->tone_left = 0;
	audio->tone_phase = 0;
	return 0;
}

/* Sets the encoder up from the codec information the sink was given. */
static int start_encoder(struct bgpd_audio *audio)
{
	if (sbc_init_a2dp(&audio->sbc, 0, audio->avdtp.sbc.info,
			  sizeof(audio->avdtp.sbc.info)) < 0)
		return -1;
	audio->sbc.bitpool = audio->avdtp.sbc.bitpool;
	audio->sbc.endian = SBC_LE;
	audio->encoding = true;
	audio->frame_pcm = sbc_get_codesize(&audio->sbc);
	audio->frame_size = sbc_get_frame_length(&audio->sbc);
	audio->frame_samples =
	    (unsigned int)(audio->frame_pcm / (CHANNELS * sizeof(int16_t)));
	audio->epoch = now_ms();
	audio->frames_sent = 0;
	audio->tone_left = (uint32_t)((uint64_t)audio->tone_ms *
				      audio->avdtp.sbc.rate / 1000U);
	audio->tone_ms = 0;
	return audio->frame_pcm > 0 && audio->frame_size > 0 &&
		       audio->frame_samples > 0
		   ? 0
		   : -1;
}

/* One frame of the test tone, as interleaved stereo samples. */
static void tone_frame(struct bgpd_audio *audio, int16_t *pcm)
{
	unsigned int rate = audio->avdtp.sbc.rate;

	for (unsigned int i = 0; i < audio->frame_samples; i++) {
		double angle = 2.0 * PI * TONE_HZ * audio->tone_phase / rate;
		int16_t sample = (int16_t)(TONE_AMPLITUDE * sin(angle));

		pcm[(size_t)i * CHANNELS] = sample;
		pcm[(size_t)i * CHANNELS + 1] = sample;
		audio->tone_phase++;
	}
	audio->tone_left = audio->tone_left > audio->frame_samples
			       ? audio->tone_left - audio->frame_samples
			       : 0;
}

/* Encodes and sends the frames the sink needs by now. */
static void stream(struct bgpd_audio *audio)
{
	struct bgpd_avdtp *a = &audio->avdtp;
	uint64_t elapsed = now_ms() - audio->epoch + LEAD_MS;
	uint64_t due = elapsed * a->sbc.rate / 1000U / audio->frame_samples;
	size_t room = bgpd_avdtp_capacity(a) / audio->frame_size;

	if (room > MAX_FRAMES_PER_PACKET)
		room = MAX_FRAMES_PER_PACKET;
	while (due > audio->frames_sent && audio->tone_left > 0) {
		uint8_t packet[MAX_FRAMES_PER_PACKET * 256];
		int16_t pcm[1024];
		size_t used = 0;
		unsigned int count = 0;

		if (room == 0 || audio->frame_pcm > sizeof(pcm) ||
		    audio->frame_size > 256)
			return;
		while (count < room && due > audio->frames_sent + count &&
		       audio->tone_left > 0) {
			ssize_t written = 0;

			tone_frame(audio, pcm);
			if (sbc_encode(&audio->sbc, pcm, audio->frame_pcm,
				       packet + used, sizeof(packet) - used,
				       &written) < 0 ||
			    written <= 0)
				return;
			used += (size_t)written;
			count++;
		}
		audio->frames_sent += count;
		if (bgpd_avdtp_send(a, packet, used, count,
				    count * audio->frame_samples) < 0)
			return;
	}
}

void bgpd_audio_poll(struct bgpd_audio *audio)
{
	struct bgpd_avdtp *a = &audio->avdtp;

	bgpd_avdtp_poll(a);
	if (a->changed) {
		snprintf(audio->status, sizeof(audio->status), "%s", a->status);
		audio->changed = true;
		a->changed = false;
	}
	if (a->state == BGPD_AVDTP_IDLE) {
		finish_encoder(audio);
		audio->tone_ms = audio->tone_left = 0;
		return;
	}
	if (a->state == BGPD_AVDTP_READY && audio->tone_ms > 0)
		bgpd_avdtp_start(a);
	if (a->state != BGPD_AVDTP_STREAMING)
		return;
	if (!audio->encoding && start_encoder(audio) < 0) {
		snprintf(audio->status, sizeof(audio->status),
			 "Cannot set up the SBC encoder");
		audio->changed = true;
		bgpd_audio_close(audio);
		return;
	}
	stream(audio);
	if (audio->tone_left == 0 && audio->frames_sent > 0) {
		finish_encoder(audio);
		bgpd_avdtp_suspend(a);
	}
}
