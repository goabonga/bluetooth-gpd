/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * hfp.h - Hands-Free Profile, audio gateway side: the control link.
 *
 * Finds the RFCOMM channel of the headset (SDP), connects to it and answers
 * its AT commands to establish the service level connection: features,
 * indicators and the codec it supports (CVSD, or mSBC for wide band). The
 * volume keys of the headset are followed. The voice itself goes through a
 * SCO link (see sco.h).
 *
 * A state machine driven by bgpd_hfp_poll(); nothing blocks.
 */
#ifndef BGPD_HFP_H
#define BGPD_HFP_H

#include <stdio.h>

#include "sdp.h"

#define BGPD_HFP_LINE 256
#define BGPD_HFP_SERVICE_HF 0x111e

enum bgpd_hfp_state {
	BGPD_HFP_IDLE,
	BGPD_HFP_SDP,	     /* looking for the RFCOMM channel */
	BGPD_HFP_CONNECTING, /* RFCOMM channel connecting */
	BGPD_HFP_SLC,	     /* exchanging the service level connection */
	BGPD_HFP_READY,	     /* service level connection established */
};

enum bgpd_hfp_codec {
	BGPD_HFP_CVSD = 1, /* narrow band, 8 kHz */
	BGPD_HFP_MSBC = 2, /* wide band, 16 kHz */
};

struct bgpd_hfp {
	enum bgpd_hfp_state state;
	struct bgpd_sdp sdp;
	int fd;
	uint8_t addr[BGPD_ADDR_SIZE];
	char name[BGPD_NAME_SIZE];
	uint64_t deadline;
	char line[BGPD_HFP_LINE]; /* AT command being received */
	size_t used;
	unsigned int features; /* of the headset (AT+BRSF) */
	bool msbc;	       /* the headset supports mSBC (AT+BAC) */
	bool codec_selected;   /* the codec of the voice link was agreed */
	unsigned int speaker, microphone; /* gains 0 to 15, -1 unknown */
	bool gain_changed;
	char status[BGPD_NAME_SIZE];
	bool changed;
	/* Opens the RFCOMM channel; the Bluetooth one unless a test sets it. */
	int (*open_channel)(const uint8_t addr[BGPD_ADDR_SIZE],
			    uint8_t channel);
};

void bgpd_hfp_init(struct bgpd_hfp *h);

/* Starts connecting to a paired Classic device that has a headset profile. */
int bgpd_hfp_connect(struct bgpd_hfp *h, const uint8_t addr[BGPD_ADDR_SIZE],
		     const char *name);

/* Uses a connected RFCOMM channel (tests). */
int bgpd_hfp_attach(struct bgpd_hfp *h, int fd, const char *name);

void bgpd_hfp_poll(struct bgpd_hfp *h);

bool bgpd_hfp_active(const struct bgpd_hfp *h);

/* The codec the voice link should use. */
enum bgpd_hfp_codec bgpd_hfp_codec(const struct bgpd_hfp *h);

/*
 * Tells the headset which codec the voice link uses (+BCS) when it was
 * negotiated. Returns -1 when the service level connection is not ready.
 */
int bgpd_hfp_select_codec(struct bgpd_hfp *h);

/* Sets the speaker or microphone gain of the headset, 0 to 15. */
int bgpd_hfp_set_speaker(struct bgpd_hfp *h, unsigned int gain);
int bgpd_hfp_set_microphone(struct bgpd_hfp *h, unsigned int gain);

void bgpd_hfp_close(struct bgpd_hfp *h);

#endif /* BGPD_HFP_H */
