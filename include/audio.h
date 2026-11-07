/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * audio.h - SBC audio streaming to a Bluetooth sink.
 *
 * Owns an AVDTP connection (see avdtp.h), encodes PCM audio with libsbc
 * and sends it in real time. The stream starts when there is audio to play
 * and is suspended when it ends. For now the audio is a test tone, played
 * with bgpd_audio_tone(); it is generated on demand, as the sink needs it.
 */
#ifndef BGPD_AUDIO_H
#define BGPD_AUDIO_H

#include <sbc/sbc.h>

#include "avdtp.h"

struct bgpd_audio {
	struct bgpd_avdtp avdtp;
	sbc_t sbc;
	bool encoding;	   /* sbc is initialised for the current stream */
	size_t frame_pcm;  /* bytes of PCM for one SBC frame */
	size_t frame_size; /* bytes of one SBC frame */
	unsigned int frame_samples; /* samples of each channel in a frame */
	uint64_t epoch;		    /* clock of the first frame */
	uint64_t frames_sent;
	uint32_t tone_ms;    /* test tone asked for, until the stream starts */
	uint32_t tone_left;  /* samples of test tone still to play */
	uint32_t tone_phase; /* position in the tone, in samples */
	char status[BGPD_AVDTP_STATUS_SIZE];
	bool changed; /* status was updated */
};

void bgpd_audio_init(struct bgpd_audio *audio);

/* Starts connecting to a paired Classic audio device. */
int bgpd_audio_connect(struct bgpd_audio *audio,
		       const uint8_t addr[BGPD_ADDR_SIZE], const char *name);

/*
 * Plays a 440 Hz tone for the given time. The stream starts as soon as the
 * connection is ready, and the tone is queued until then.
 */
int bgpd_audio_tone(struct bgpd_audio *audio, unsigned int milliseconds);

/* Advances the connection and sends the audio that is due. */
void bgpd_audio_poll(struct bgpd_audio *audio);

/* True while the connection is open or opening. */
bool bgpd_audio_active(const struct bgpd_audio *audio);

/* True while audio is being sent, and the caller should poll often. */
bool bgpd_audio_streaming(const struct bgpd_audio *audio);

/* Stops the stream and closes the connection. */
void bgpd_audio_close(struct bgpd_audio *audio);

#endif /* BGPD_AUDIO_H */
