/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * avdtp.h - source side of A2DP: AVDTP signaling and media transport.
 *
 * Connects to a Classic audio sink, discovers its endpoints, picks an SBC
 * configuration, opens the media channel and then sends media packets. It
 * is a state machine driven by bgpd_avdtp_poll(): one signaling command is
 * in flight at a time and nothing blocks.
 */
#ifndef BGPD_AVDTP_H
#define BGPD_AVDTP_H

#include <stdio.h>

#include "device.h"

#define BGPD_AVDTP_MAX_SEPS 16
#define BGPD_AVDTP_STATUS_SIZE 256
#define BGPD_AVDTP_DEFAULT_MTU 672

enum bgpd_avdtp_state {
	BGPD_AVDTP_IDLE,
	BGPD_AVDTP_CONNECTING, /* signaling channel */
	BGPD_AVDTP_DISCOVER,
	BGPD_AVDTP_CAPABILITIES,
	BGPD_AVDTP_CONFIGURE,
	BGPD_AVDTP_OPEN,
	BGPD_AVDTP_MEDIA, /* media channel connecting */
	BGPD_AVDTP_READY, /* configured and open, not streaming */
	BGPD_AVDTP_STARTING,
	BGPD_AVDTP_STREAMING,
	BGPD_AVDTP_SUSPENDING,
};

/* SBC configuration chosen with the sink (A2DP codec information). */
struct bgpd_avdtp_sbc {
	unsigned int rate;    /* 44100 or 48000 */
	uint8_t channel_mode; /* 8 mono, 4 dual, 2 stereo, 1 joint stereo */
	uint8_t bitpool;
	bool loudness;	 /* loudness allocation, else SNR */
	uint8_t info[4]; /* the A2DP SBC codec information, as configured */
};

struct bgpd_avdtp {
	enum bgpd_avdtp_state state;
	int sig, media; /* signaling and media channels */
	uint8_t addr[BGPD_ADDR_SIZE];
	char name[BGPD_NAME_SIZE];
	uint8_t label, signal; /* of the command in flight */
	uint8_t seps[BGPD_AVDTP_MAX_SEPS];
	size_t sep_count, sep_next;
	uint8_t seid; /* remote endpoint in use */
	struct bgpd_avdtp_sbc sbc;
	uint16_t mtu, seq;
	uint32_t timestamp, ssrc;
	uint64_t deadline;
	char status[BGPD_AVDTP_STATUS_SIZE]; /* last message */
	bool changed;			     /* status was updated */
	/* Opens a channel to the sink; the Bluetooth one unless a test sets it.
	 */
	int (*open_channel)(const uint8_t addr[BGPD_ADDR_SIZE]);
	FILE *trace;
	uint64_t trace_start;
};

void bgpd_avdtp_init(struct bgpd_avdtp *a);

/* Starts connecting to a paired Classic device. */
int bgpd_avdtp_connect(struct bgpd_avdtp *a, const uint8_t addr[BGPD_ADDR_SIZE],
		       const char *name);

/* Advances the state machine with what the sink sent. */
void bgpd_avdtp_poll(struct bgpd_avdtp *a);

/* True while the channels are open or opening. */
bool bgpd_avdtp_active(const struct bgpd_avdtp *a);

/* Asks the sink to start or suspend the stream; the state then follows. */
int bgpd_avdtp_start(struct bgpd_avdtp *a);
int bgpd_avdtp_suspend(struct bgpd_avdtp *a);

/*
 * Sends frames of SBC audio (count frames, size bytes) that hold samples
 * samples of each channel. Needs the STREAMING state; returns -1 when the
 * packet does not fit the channel or could not be sent.
 */
int bgpd_avdtp_send(struct bgpd_avdtp *a, const uint8_t *frames, size_t size,
		    unsigned int count, unsigned int samples);

/* Largest number of bytes of frames one packet can carry. */
size_t bgpd_avdtp_capacity(const struct bgpd_avdtp *a);

/* Closes the stream and both channels. */
void bgpd_avdtp_close(struct bgpd_avdtp *a);

#endif /* BGPD_AVDTP_H */
