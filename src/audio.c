/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "audio.h"

#include "streams.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

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
	audio->client = -1;
	audio->volume = 100;
	bgpd_avdtp_init(&audio->avdtp);
	bgpd_avrcp_init(&audio->avrcp);
}

bool bgpd_audio_active(const struct bgpd_audio *audio)
{
	return bgpd_avdtp_active(&audio->avdtp);
}

bool bgpd_audio_streaming(const struct bgpd_audio *audio)
{
	return audio->avdtp.state == BGPD_AVDTP_STREAMING;
}

static void drop_client(struct bgpd_audio *audio)
{
	if (audio->client >= 0)
		close(audio->client);
	audio->client = -1;
}

int bgpd_audio_attach(struct bgpd_audio *audio, int fd)
{
	if (audio->client >= 0 || audio->avdtp.state < BGPD_AVDTP_READY ||
	    audio->avdtp.sbc.rate == 0 ||
	    !bgpd_stream_accept(fd, audio->avdtp.sbc.rate)) {
		bgpd_stream_refuse(fd);
		return -1;
	}
	audio->client = fd;
	return 0;
}

/* Takes the PCM the client sent into the ring. */
static void read_client(struct bgpd_audio *audio)
{
	while (audio->client >= 0) {
		size_t tail =
		    (audio->ring_head + audio->ring_used) % BGPD_AUDIO_RING;
		size_t room = BGPD_AUDIO_RING - audio->ring_used;
		ssize_t n;

		if (room == 0)
			return; /* the client waits */
		if (room > BGPD_AUDIO_RING - tail)
			room = BGPD_AUDIO_RING - tail;
		n = read(audio->client, audio->ring + tail, room);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			drop_client(audio);
			return;
		}
		audio->ring_used += (size_t)n;
	}
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
	bgpd_avrcp_close(&audio->avrcp);
	audio->hw_volume = audio->avrcp_tried = false;
	audio->tone_ms = audio->tone_left = 0;
	drop_client(audio);
	audio->ring_head = audio->ring_used = 0;
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
	audio->sent_any = false;
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

void bgpd_audio_set_volume(struct bgpd_audio *audio, unsigned int percent)
{
	audio->volume = percent > 100U ? 100U : percent;
	if (audio->hw_volume)
		bgpd_avrcp_set_volume(&audio->avrcp, audio->volume);
}

/* -50 dB at 1 % up to 0 dB at 100 %, silence at 0. */
static double gain(unsigned int percent)
{
	if (percent == 0)
		return 0.0;
	return pow(10.0, -50.0 * (100.0 - percent) / 99.0 / 20.0);
}

static void apply_volume(const struct bgpd_audio *audio, int16_t *pcm)
{
	double g = gain(audio->volume);

	if (audio->hw_volume || audio->volume >= 100U)
		return;
	for (size_t i = 0; i < (size_t)audio->frame_samples * CHANNELS; i++)
		pcm[i] = (int16_t)(pcm[i] * g);
}

/* True when a frame of audio can be taken from the tone or the client. */
static bool have_frame(const struct bgpd_audio *audio)
{
	return audio->tone_left > 0 || audio->ring_used >= audio->frame_pcm;
}

/* Takes the next frame of PCM: the tone first, else what the client sent. */
static void next_frame(struct bgpd_audio *audio, int16_t *pcm)
{
	uint8_t *bytes = (uint8_t *)pcm;

	if (audio->tone_left > 0) {
		tone_frame(audio, pcm);
		apply_volume(audio, pcm);
		return;
	}
	for (size_t i = 0; i < audio->frame_pcm; i++) {
		bytes[i] = audio->ring[audio->ring_head];
		audio->ring_head = (audio->ring_head + 1U) % BGPD_AUDIO_RING;
	}
	audio->ring_used -= audio->frame_pcm;
	apply_volume(audio, pcm);
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
	if (!have_frame(audio) && audio->sent_any && due > audio->frames_sent) {
		/* The client is late: do not send a burst when it catches up.
		 */
		audio->epoch = now_ms();
		audio->frames_sent = 0;
		return;
	}
	while (due > audio->frames_sent && have_frame(audio)) {
		uint8_t packet[MAX_FRAMES_PER_PACKET * 256];
		int16_t pcm[1024] = {0};
		size_t used = 0;
		unsigned int count = 0;

		if (room == 0 || audio->frame_pcm > sizeof(pcm) ||
		    audio->frame_size > 256)
			return;
		while (count < room && due > audio->frames_sent + count &&
		       have_frame(audio)) {
			ssize_t written = 0;

			next_frame(audio, pcm);
			if (sbc_encode(&audio->sbc, pcm, audio->frame_pcm,
				       packet + used, sizeof(packet) - used,
				       &written) < 0 ||
			    written <= 0)
				return;
			used += (size_t)written;
			count++;
		}
		audio->frames_sent += count;
		audio->sent_any = true;
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
	read_client(audio);
	/* The control channel gives the volume of the device, when it has one.
	 */
	if (!audio->avrcp_tried && a->state >= BGPD_AVDTP_READY) {
		audio->avrcp_tried = true;
		bgpd_avrcp_connect(&audio->avrcp, a->addr);
	}
	bgpd_avrcp_poll(&audio->avrcp);
	if (audio->avrcp.changed) {
		audio->avrcp.changed = false;
		audio->hw_volume = audio->avrcp.supported;
		if (audio->hw_volume && audio->avrcp.volume >= 0)
			audio->volume = (unsigned int)audio->avrcp.volume;
	}
	if (a->state == BGPD_AVDTP_IDLE) {
		bgpd_avrcp_close(&audio->avrcp);
		audio->hw_volume = audio->avrcp_tried = false;
		finish_encoder(audio);
		audio->tone_ms = audio->tone_left = 0;
		drop_client(audio);
		audio->ring_head = audio->ring_used = 0;
		return;
	}
	if (a->state == BGPD_AVDTP_READY &&
	    (audio->tone_ms > 0 || audio->ring_used > 0))
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
	if (audio->sent_any && audio->tone_left == 0 && audio->client < 0 &&
	    audio->ring_used < audio->frame_pcm) {
		finish_encoder(audio);
		/* The end of a frame left over would restart the stream. */
		audio->ring_head = audio->ring_used = 0;
		bgpd_avdtp_suspend(a);
	}
}
