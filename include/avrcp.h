/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * avrcp.h - hardware volume of an audio device (AVRCP Absolute Volume).
 *
 * Connects to the AVCTP channel of a headset or speaker and acts as the
 * controller: it asks to be told when the volume changes (so the buttons
 * of the device are followed) and sets the volume of the device. A device
 * that does not support it answers "not implemented", and then the volume
 * has to be applied in software.
 *
 * It is a state machine driven by bgpd_avrcp_poll(); nothing blocks.
 */
#ifndef BGPD_AVRCP_H
#define BGPD_AVRCP_H

#include <stdio.h>

#include "device.h"

#define BGPD_AVRCP_STATUS_SIZE 256

enum bgpd_avrcp_state {
	BGPD_AVRCP_IDLE,
	BGPD_AVRCP_CONNECTING,
	BGPD_AVRCP_ACTIVE, /* connected: waiting for, or following, the volume
			    */
};

struct bgpd_avrcp {
	enum bgpd_avrcp_state state;
	int fd;
	uint8_t addr[BGPD_ADDR_SIZE];
	uint8_t label;	/* of the last command */
	bool supported; /* the device has an absolute volume */
	int volume;	/* 0 to 100, or -1 when not known */
	bool changed;	/* volume or support changed */
	uint64_t deadline;
	char status[BGPD_AVRCP_STATUS_SIZE];
	/* Opens the channel; the Bluetooth one unless a test sets it. */
	int (*open_channel)(const uint8_t addr[BGPD_ADDR_SIZE]);
};

void bgpd_avrcp_init(struct bgpd_avrcp *a);

/* Starts connecting to a paired Classic device. */
int bgpd_avrcp_connect(struct bgpd_avrcp *a,
		       const uint8_t addr[BGPD_ADDR_SIZE]);

/* Reads what the device sent and keeps the volume up to date. */
void bgpd_avrcp_poll(struct bgpd_avrcp *a);

bool bgpd_avrcp_active(const struct bgpd_avrcp *a);

/* Sets the volume of the device, 0 to 100. Returns -1 when it has none. */
int bgpd_avrcp_set_volume(struct bgpd_avrcp *a, unsigned int percent);

void bgpd_avrcp_close(struct bgpd_avrcp *a);

#endif /* BGPD_AVRCP_H */
