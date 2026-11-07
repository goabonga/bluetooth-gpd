/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * ipc.h - control protocol between the daemon and the interface.
 *
 * They talk over a Unix stream socket in text lines. The daemon sends a
 * snapshot of its state whenever it changes; the interface sends one
 * command per line. A snapshot is a series of tab-separated records ended
 * by an "end" line, and it replaces the previous one.
 */
#ifndef BGPD_IPC_H
#define BGPD_IPC_H

#include <stddef.h>

#include "mgmt.h"

/* Largest snapshot: the full device table and the fixed records. */
#define BGPD_IPC_SNAPSHOT_MAX 196608

enum bgpd_ipc_kind {
	BGPD_IPC_SCAN,
	BGPD_IPC_POWER,
	BGPD_IPC_ADAPTER,
	BGPD_IPC_CANCEL,
	BGPD_IPC_ACCEPT, /* answers the open prompt, with input */
	BGPD_IPC_REJECT,
	BGPD_IPC_PAIR,
	BGPD_IPC_CONNECT,
	BGPD_IPC_DISCONNECT,
	BGPD_IPC_UNPAIR,
};

struct bgpd_ipc_command {
	enum bgpd_ipc_kind kind;
	uint8_t addr[BGPD_ADDR_SIZE]; /* PAIR, CONNECT, DISCONNECT, UNPAIR */
	char input[BGPD_INPUT_SIZE];  /* ACCEPT: PIN or passkey, or empty */
};

/*
 * Writes the snapshot of the state of m, a NUL-terminated string. Returns
 * its length, or 0 when it does not fit in size bytes.
 */
size_t bgpd_ipc_snapshot(const struct bgpd_mgmt *m, char *out, size_t size);

/*
 * Copies a snapshot into m, the copy of the daemon's state kept by the
 * interface. Returns -1 and leaves m untouched when the snapshot is
 * malformed. The input being typed (m->input) is not changed.
 */
int bgpd_ipc_apply(struct bgpd_mgmt *m, const char *snapshot);

/* Writes the line of a command, newline included. Returns its length. */
size_t bgpd_ipc_format(const struct bgpd_ipc_command *command, char *out,
		       size_t size);

/* Reads one line, without its newline. Returns -1 when it is not valid. */
int bgpd_ipc_parse(const char *line, struct bgpd_ipc_command *command);

#endif /* BGPD_IPC_H */
