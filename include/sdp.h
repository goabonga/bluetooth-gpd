/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * sdp.h - finds the RFCOMM channel of a service of a Classic device.
 *
 * Asks the SDP server of the device which RFCOMM channel offers a service
 * (for instance Hands-Free, 0x111e) with one ServiceSearchAttributeRequest
 * for the protocol descriptor list. A state machine driven by
 * bgpd_sdp_poll(); nothing blocks.
 */
#ifndef BGPD_SDP_H
#define BGPD_SDP_H

#include "device.h"

#define BGPD_SDP_BUFFER 4096

enum bgpd_sdp_state {
	BGPD_SDP_IDLE,
	BGPD_SDP_CONNECTING,
	BGPD_SDP_WAITING, /* the request was sent */
	BGPD_SDP_FOUND,
	BGPD_SDP_FAILED,
};

struct bgpd_sdp {
	enum bgpd_sdp_state state;
	int fd;
	uint16_t service; /* UUID of the service looked for */
	uint16_t transaction;
	uint8_t channel; /* the result, when FOUND */
	uint64_t deadline;
	size_t used;
	uint8_t response[BGPD_SDP_BUFFER]; /* attribute lists received */
	uint8_t continuation[16];	   /* continuation state to send back */
	size_t continuation_size;
	/* Opens the channel; the Bluetooth one unless a test sets it. */
	int (*open_channel)(const uint8_t addr[BGPD_ADDR_SIZE]);
};

void bgpd_sdp_init(struct bgpd_sdp *q);

/* Starts looking for the service (16-bit UUID) on a Classic device. */
int bgpd_sdp_start(struct bgpd_sdp *q, const uint8_t addr[BGPD_ADDR_SIZE],
		   uint16_t service);

/* Advances the query; true when it is over, found or failed. */
bool bgpd_sdp_poll(struct bgpd_sdp *q);

void bgpd_sdp_close(struct bgpd_sdp *q);

/*
 * Reads the RFCOMM channel out of attribute lists (a data element
 * sequence). Returns 0 when there is one, -1 when there is none or the
 * data is malformed.
 */
int bgpd_sdp_parse(const uint8_t *lists, size_t size, uint8_t *channel);

#endif /* BGPD_SDP_H */
