/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * voice.h - the microphone of the headset, served to a capture client.
 *
 * A capture client (the ALSA capture plugin, see streams.h for how it gets
 * here) triggers the whole chain: the hands-free link to the headset, the
 * codec negotiation and the SCO voice link. The client is then told the
 * sample rate (8000 for CVSD, 16000 for mSBC), or 0 when there is no
 * headset or the link failed, and receives the microphone as signed 16-bit
 * little-endian mono PCM at that rate until it leaves.
 */
#ifndef BGPD_VOICE_H
#define BGPD_VOICE_H

#include "hfp.h"
#include "sco.h"

enum bgpd_voice_state {
	BGPD_VOICE_IDLE,
	BGPD_VOICE_HANDSFREE, /* control link being established */
	BGPD_VOICE_CODEC,     /* waiting for the headset to confirm the codec */
	BGPD_VOICE_LINK,      /* voice link connecting */
	BGPD_VOICE_STREAMING,
};

struct bgpd_voice {
	enum bgpd_voice_state state;
	struct bgpd_hfp hfp;
	struct bgpd_sco sco;
	int client;	 /* the client the microphone is for, or -1 */
	bool has_target; /* the headset in use */
	uint8_t target[BGPD_ADDR_SIZE];
	char target_name[BGPD_NAME_SIZE];
	uint64_t deadline;
	char status[BGPD_NAME_SIZE];
	bool changed;
};

void bgpd_voice_init(struct bgpd_voice *v);

/* The headset a capture client gets, or none (name is ignored then). */
void bgpd_voice_target(struct bgpd_voice *v, const uint8_t addr[BGPD_ADDR_SIZE],
		       const char *name);
void bgpd_voice_no_target(struct bgpd_voice *v);

/*
 * Gives the microphone to a client whose greeting was read (see
 * streams.h): the links are set up, then the client is told the rate and
 * receives the PCM. It is refused (rate 0) and closed when there is no
 * headset, or a client already. Returns -1 then.
 */
int bgpd_voice_attach(struct bgpd_voice *v, int fd);

void bgpd_voice_poll(struct bgpd_voice *v);

/* True while the microphone is sent, and the caller should poll often. */
bool bgpd_voice_streaming(const struct bgpd_voice *v);

/* Ends the client and the links. */
void bgpd_voice_shutdown(struct bgpd_voice *v);

#endif /* BGPD_VOICE_H */
