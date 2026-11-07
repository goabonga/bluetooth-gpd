/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * daemon.h - keeps the paired BLE devices connected.
 *
 * The daemon owns the management socket and one HID-over-GATT connection
 * per paired LE device. When a link drops or a device is out of range, the
 * connection is tried again after a delay, so the devices stay usable
 * without any interface running.
 */
#ifndef BGPD_DAEMON_H
#define BGPD_DAEMON_H

#include <stdio.h>

#include "audio.h"
#include "hog.h"
#include "ipc.h"
#include "mgmt.h"

#define BGPD_DAEMON_SLOTS 8
#define BGPD_DAEMON_RETRY_MS 5000

struct bgpd_daemon_slot {
	bool used;
	bool held; /* disconnected on purpose: no automatic connection */
	uint8_t addr[BGPD_ADDR_SIZE];
	uint64_t retry; /* bgpd_now() of the next connection attempt */
	struct bgpd_hog hog;
};

struct bgpd_daemon {
	struct bgpd_mgmt *mgmt;
	FILE *log;		       /* messages, or NULL */
	char logged[BGPD_STATUS_SIZE]; /* the last message written to log */
	struct bgpd_daemon_slot slots[BGPD_DAEMON_SLOTS];
	struct bgpd_audio audio; /* the Classic audio device in use */
	struct bgpd_ipc_server ipc;
};

void bgpd_daemon_init(struct bgpd_daemon *daemon, struct bgpd_mgmt *mgmt,
		      FILE *log);

/*
 * Starts serving the control socket at path: interfaces then see the state
 * and send commands (scan, pair, connect...) through bgpd_daemon_poll().
 */
int bgpd_daemon_listen(struct bgpd_daemon *daemon, const char *path);

/*
 * Reads the management socket, serves the interfaces, then advances every
 * connection and starts the ones that are due. Nothing blocks.
 */
void bgpd_daemon_poll(struct bgpd_daemon *daemon);

/*
 * Writes the status of the manager to the log when it is not the last
 * message the log received.
 */
void bgpd_daemon_log_changes(struct bgpd_daemon *daemon);

/* True while audio is sent, and the caller should poll often. */
bool bgpd_daemon_streaming(const struct bgpd_daemon *daemon);

/* Number of devices currently serving input. */
size_t bgpd_daemon_running(const struct bgpd_daemon *daemon);

/* Drops every connection. */
void bgpd_daemon_close(struct bgpd_daemon *daemon);

#endif /* BGPD_DAEMON_H */
