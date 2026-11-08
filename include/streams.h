/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * streams.h - accepts the PCM clients of the audio and capture sockets.
 *
 * A client connects, then sends a greeting that says which audio device it
 * wants: "BGPDPCM2" for playback or "BGPDCAP2" for the microphone, followed
 * by the address of the device (14 hexadecimal digits, the form of the
 * control protocol; all zeros for the device last connected) and a newline.
 * The daemon answers with the sample rate (4 bytes, little-endian), or 0
 * when the stream cannot be served, and then audio flows: playback is
 * stereo 16-bit PCM from the client, capture is mono 16-bit PCM to it.
 *
 * Only clients of the same user are accepted, on sockets only that user can
 * use. Nothing blocks.
 */
#ifndef BGPD_STREAMS_H
#define BGPD_STREAMS_H

#include "device.h"

#define BGPD_STREAMS_PENDING 8
#define BGPD_STREAMS_GREETING 23U /* 8 + 14 + newline */

enum bgpd_stream_kind {
	BGPD_STREAM_PLAYBACK,
	BGPD_STREAM_CAPTURE,
};

/* A client whose greeting was read. */
struct bgpd_stream_request {
	int fd;
	enum bgpd_stream_kind kind;
	uint8_t addr[BGPD_ADDR_SIZE];
	bool any; /* no device asked for: the one last connected */
};

struct bgpd_stream_client {
	int fd;
	enum bgpd_stream_kind kind;
	char greeting[BGPD_STREAMS_GREETING];
	size_t used;
	uint64_t deadline;
};

struct bgpd_streams {
	int listener[2]; /* indexed by enum bgpd_stream_kind */
	char path[2][108];
	struct bgpd_stream_client pending[BGPD_STREAMS_PENDING];
};

void bgpd_streams_init(struct bgpd_streams *s);

/*
 * bluetooth-audio.sock or bluetooth-capture.sock in $BLUETOOTH_RUNTIME_DIR (an
 * absolute path), else in /run/bluetooth.
 */
int bgpd_streams_default_path(enum bgpd_stream_kind kind, char *buffer,
			      size_t size);

/* Listens on a Unix socket for the clients of one kind of stream. */
int bgpd_streams_listen(struct bgpd_streams *s, enum bgpd_stream_kind kind,
			const char *path);

/*
 * Accepts new clients and reads their greetings. Returns true and fills
 * request when one is complete: the caller then owns request->fd. Call it
 * again for the next one.
 */
bool bgpd_streams_next(struct bgpd_streams *s,
		       struct bgpd_stream_request *request);

/* Tells a client the stream cannot be served (rate 0) and closes it. */
void bgpd_stream_refuse(int fd);

/* Tells a client the sample rate of the stream. */
bool bgpd_stream_accept(int fd, unsigned int rate);

/* Closes the clients that never completed their greeting and the sockets. */
void bgpd_streams_shutdown(struct bgpd_streams *s);

#endif /* BGPD_STREAMS_H */
