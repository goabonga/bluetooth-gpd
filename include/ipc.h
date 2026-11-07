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
#include <sys/types.h>

#include "mgmt.h"

/* Largest snapshot: the full device table and the fixed records. */
#define BGPD_IPC_SNAPSHOT_MAX 196608

enum bgpd_ipc_kind {
	BGPD_IPC_SCAN,
	BGPD_IPC_POWER,
	BGPD_IPC_ADAPTER,
	BGPD_IPC_VISIBLE, /* makes the adapter visible, or hides it */
	BGPD_IPC_CANCEL,
	BGPD_IPC_ACCEPT, /* answers the open prompt, with input */
	BGPD_IPC_REJECT,
	BGPD_IPC_PAIR,
	BGPD_IPC_CONNECT,
	BGPD_IPC_DISCONNECT,
	BGPD_IPC_UNPAIR,
	BGPD_IPC_TONE,	 /* plays a test tone on an audio device */
	BGPD_IPC_VOLUME, /* sets the volume of the audio device in use */
};

struct bgpd_ipc_command {
	enum bgpd_ipc_kind kind;
	uint8_t addr[BGPD_ADDR_SIZE]; /* PAIR, CONNECT, DISCONNECT, UNPAIR */
	char input[BGPD_INPUT_SIZE];  /* ACCEPT: PIN or passkey, or empty */
	int value;		      /* VOLUME: 0 to 100 */
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

#define BGPD_IPC_MAX_CLIENTS 4
#define BGPD_IPC_LINE_MAX 128

struct bgpd_ipc_peer {
	int fd;
	char line[BGPD_IPC_LINE_MAX]; /* start of the line being received */
	size_t used;
};

struct bgpd_ipc_server {
	int listener;
	struct bgpd_ipc_peer peers[BGPD_IPC_MAX_CLIENTS];
	char last[BGPD_IPC_SNAPSHOT_MAX]; /* snapshot sent last */
	char work[BGPD_IPC_SNAPSHOT_MAX];
	size_t last_size;
	char path[108]; /* the socket file, removed on close */
};

typedef void (*bgpd_ipc_handler)(void *context,
				 const struct bgpd_ipc_command *command);

/* $XDG_RUNTIME_DIR/bluetooth.sock; -1 when there is no such directory. */
int bgpd_ipc_default_path(char *buffer, size_t size);

/*
 * Starts listening on a Unix socket only its owner can use. A socket left
 * by a stopped daemon is replaced; one that answers makes it fail with
 * EADDRINUSE.
 */
int bgpd_ipc_listen(struct bgpd_ipc_server *server, const char *path);

/*
 * Accepts clients, passes their commands to handler and sends the snapshot
 * of m to all of them when it changed. Nothing blocks: a client that cannot
 * keep up is dropped and reconnects. Clients of another user are refused.
 */
void bgpd_ipc_serve(struct bgpd_ipc_server *server, const struct bgpd_mgmt *m,
		    bgpd_ipc_handler handler, void *context);

/* Number of connected clients. */
size_t bgpd_ipc_clients(const struct bgpd_ipc_server *server);

/* Disconnects the clients and removes the socket. */
void bgpd_ipc_server_close(struct bgpd_ipc_server *server);

#define BGPD_IPC_RECONNECT_MS 1000

struct bgpd_ipc_client {
	int fd;
	char path[108];
	uint64_t retry; /* bgpd_now() of the next connection attempt */
	size_t used;
	char buffer[BGPD_IPC_SNAPSHOT_MAX + 1]; /* snapshot being received */
};

/* Prepares a client of the daemon listening on path; connects at once. */
void bgpd_ipc_connect(struct bgpd_ipc_client *client, const char *path);

/*
 * Applies to mirror the snapshots the daemon sent. When the daemon cannot
 * be reached the mirror is marked not ready, and the connection is tried
 * again every second. Returns true while connected.
 */
bool bgpd_ipc_poll(struct bgpd_ipc_client *client, struct bgpd_mgmt *mirror);

/* Sends a command. Returns -1 when not connected. */
int bgpd_ipc_send(struct bgpd_ipc_client *client,
		  const struct bgpd_ipc_command *command);

void bgpd_ipc_client_close(struct bgpd_ipc_client *client);

#endif /* BGPD_IPC_H */
