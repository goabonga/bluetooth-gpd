/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * sco.h - the voice link of a headset (SCO), microphone and earpiece.
 *
 * Opens the SCO link to a paired headset and turns it into two mono 16-bit
 * PCM streams: what the microphone picks up, and what is played in the
 * earpiece. The link carries CVSD (8 kHz) or mSBC (16 kHz, wide band, one
 * 60 byte packet for each frame of 120 samples). The codec is the one
 * negotiated with the headset (see hfp.h).
 *
 * Driven by bgpd_sco_poll(); nothing blocks.
 */
#ifndef BGPD_SCO_H
#define BGPD_SCO_H

#include <sbc/sbc.h>

#include "device.h"
#include "hfp.h"

#define BGPD_SCO_RING 32768U /* samples of each PCM ring */

struct bgpd_sco {
	int fd;
	enum bgpd_hfp_codec codec;
	unsigned int rate;	/* 8000 or 16000 */
	uint16_t mtu;		/* bytes of a packet of the link */
	bool opened;		/* the link is connected */
	sbc_t encoder, decoder; /* mSBC */
	bool sbc_ready;
	uint8_t pending[128]; /* bytes received, not yet a frame */
	size_t pending_size;
	uint8_t outgoing[512]; /* bytes encoded, not yet written */
	size_t outgoing_size;
	unsigned int sequence; /* of the next mSBC packet */
	uint64_t deadline;
	int16_t mic[BGPD_SCO_RING];
	size_t mic_head, mic_used;
	int16_t speaker[BGPD_SCO_RING];
	size_t speaker_head, speaker_used;
	/* Opens the link; the Bluetooth one unless a test sets it. */
	int (*open_link)(const uint8_t addr[BGPD_ADDR_SIZE],
			 enum bgpd_hfp_codec codec);
};

void bgpd_sco_init(struct bgpd_sco *s);

/* Starts connecting the voice link to a headset. */
int bgpd_sco_open(struct bgpd_sco *s, const uint8_t addr[BGPD_ADDR_SIZE],
		  enum bgpd_hfp_codec codec);

/* Moves the audio between the link and the rings; false when it ended. */
bool bgpd_sco_poll(struct bgpd_sco *s);

bool bgpd_sco_active(const struct bgpd_sco *s);

/* Takes up to count samples of microphone; returns how many. */
size_t bgpd_sco_read(struct bgpd_sco *s, int16_t *samples, size_t count);

/* Queues up to count samples for the earpiece; returns how many. */
size_t bgpd_sco_write(struct bgpd_sco *s, const int16_t *samples, size_t count);

void bgpd_sco_close(struct bgpd_sco *s);

#endif /* BGPD_SCO_H */
