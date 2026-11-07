/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * hog.h - HID over GATT host (keyboards, mice, game controllers in BLE).
 *
 * Keeps the LE link of a device alive through an ATT channel, finds its
 * HID service, reads the report map, subscribes to the input reports and
 * forwards them to the kernel through uhid, so that the device works as a
 * local input device. No BlueZ service is involved.
 *
 * The object is a state machine driven by bgpd_hog_poll(): one ATT request
 * is in flight at a time and nothing blocks.
 */
#ifndef BGPD_HOG_H
#define BGPD_HOG_H

#include <stdio.h>

#include "device.h"

#define BGPD_HOG_MAX_CHARS 24
#define BGPD_HOG_MAP_SIZE 4096
#define BGPD_HOG_STATUS_SIZE 256

enum bgpd_hog_state {
	BGPD_HOG_IDLE,
	BGPD_HOG_CONNECTING,
	BGPD_HOG_SERVICES,
	BGPD_HOG_CHARACTERISTICS,
	BGPD_HOG_DESCRIPTORS,
	BGPD_HOG_MAP,
	BGPD_HOG_REFERENCES,
	BGPD_HOG_SUBSCRIBE,
	BGPD_HOG_RUNNING,
};

struct bgpd_hog_char {
	uint16_t uuid, props, value, cccd, reference;
	uint8_t report_id, report_type;
};

struct bgpd_hog {
	enum bgpd_hog_state state;
	int att, uhid;
	const char *uhid_path; /* "/dev/uhid" unless a test changes it */
	char name[BGPD_NAME_SIZE];
	uint16_t service_start, service_end, cursor;
	struct bgpd_hog_char chars[BGPD_HOG_MAX_CHARS];
	size_t count, next;
	uint8_t map[BGPD_HOG_MAP_SIZE];
	size_t map_size;
	uint64_t deadline;
	char status[BGPD_HOG_STATUS_SIZE]; /* last message */
	bool changed;			   /* status was updated */
	FILE *trace;			   /* log of the steps, or NULL */
	uint64_t trace_start;		   /* clock of the log's time zero */
};

void bgpd_hog_init(struct bgpd_hog *hog);

/* Starts connecting to a bonded LE device. */
int bgpd_hog_connect(struct bgpd_hog *hog, const uint8_t addr[BGPD_ADDR_SIZE],
		     const char *name);

/* Uses an ATT channel that is already connected (tests). */
int bgpd_hog_attach(struct bgpd_hog *hog, int att, const char *name);

/* Advances the state machine with what the device sent. */
void bgpd_hog_poll(struct bgpd_hog *hog);

/* True while the channel is open or opening. */
bool bgpd_hog_active(const struct bgpd_hog *hog);

/* Drops the link and the local input device. */
void bgpd_hog_close(struct bgpd_hog *hog);

#endif /* BGPD_HOG_H */
