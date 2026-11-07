/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "bgpd.h"
#include "daemon.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USAGE                                                                  \
	"Usage: bluetooth-daemon [--adapter hci0] [--state-dir DIR] "          \
	"[--socket PATH] [--audio-socket PATH] [--capture-socket PATH] "       \
	"[--trace FILE]\n"                                                     \
	"Keeps the paired Bluetooth devices connected through the Linux "      \
	"kernel and serves the interfaces on a Unix socket.\n"

static volatile sig_atomic_t quitting;

static void interrupted(int sig)
{
	(void)sig;
	quitting = 1;
}

/* Accepts "hci1" or "1". */
static bool adapter_index(const char *text, uint16_t *index)
{
	char *end;
	unsigned long n;

	if (strncmp(text, "hci", 3) == 0)
		text += 3;
	if (text[0] < '0' || text[0] > '9')
		return false;
	errno = 0;
	n = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || n >= BGPD_NO_INDEX)
		return false;
	*index = (uint16_t)n;
	return true;
}

static void run(struct bgpd_daemon *daemon)
{
	struct pollfd p = {.fd = daemon->mgmt->fd, .events = POLLIN};

	while (!quitting) {
		bgpd_daemon_poll(daemon);
		bgpd_daemon_log_changes(daemon);
		/* Wakes on kernel events, and often enough for the HID links
		 * and, while audio is sent, for its packets. */
		if (poll(&p, 1, bgpd_daemon_streaming(daemon) ? 5 : 50) < 0 &&
		    errno != EINTR)
			break;
	}
}

int main(int argc, char **argv)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_daemon *daemon = calloc(1, sizeof(*daemon));
	char state_dir[4096];
	uint16_t adapter = BGPD_NO_INDEX;
	char socket_path[108], audio_path[108], capture_path[108];
	const char *dir = NULL, *trace = NULL, *sock = NULL, *audio_sock = NULL,
		   *capture_sock = NULL;
	int status = 0;

	if (m == NULL || daemon == NULL) {
		free(daemon);
		free(m);
		return 1;
	}
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_daemon_init(daemon, m, stderr);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			puts(BGPD_VERSION);
			goto done;
		} else if (strcmp(argv[i], "--help") == 0 ||
			   strcmp(argv[i], "-h") == 0) {
			fputs(USAGE, stdout);
			goto done;
		} else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
			trace = argv[++i];
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			sock = argv[++i];
		} else if (strcmp(argv[i], "--audio-socket") == 0 &&
			   i + 1 < argc) {
			audio_sock = argv[++i];
		} else if (strcmp(argv[i], "--capture-socket") == 0 &&
			   i + 1 < argc) {
			capture_sock = argv[++i];
		} else if (strcmp(argv[i], "--state-dir") == 0 &&
			   i + 1 < argc) {
			dir = argv[++i];
		} else if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc &&
			   adapter_index(argv[i + 1], &adapter)) {
			i++;
		} else {
			fprintf(stderr,
				"Unknown or incomplete argument: %s\n%s",
				argv[i], USAGE);
			status = 2;
			goto done;
		}
	}
	if (dir == NULL) {
		if (bgpd_store_default_dir(state_dir, sizeof(state_dir)) < 0) {
			fprintf(stderr, "No directory for the keys: %s\n",
				strerror(errno));
			status = 1;
			goto done;
		}
		dir = state_dir;
	}
	bgpd_mgmt_init(m, adapter, dir);
	if (trace != NULL && bgpd_mgmt_trace(m, trace) < 0) {
		fprintf(stderr, "Trace file %s: %s\n", trace, strerror(errno));
		status = 1;
		goto done;
	}
	if (bgpd_mgmt_open(m) < 0) {
		fprintf(stderr, "%s\n", m->status);
		status = 1;
		goto done;
	}
	bgpd_mgmt_check_manager(m, "/proc");
	if (m->warning[0] != '\0')
		fprintf(stderr, "%s\n", m->warning);
	signal(SIGINT, interrupted);
	signal(SIGTERM, interrupted);
	signal(SIGPIPE, SIG_IGN);
	if (sock == NULL) {
		if (bgpd_ipc_default_path(socket_path, sizeof(socket_path)) <
		    0) {
			fprintf(stderr, "No XDG_RUNTIME_DIR for the socket\n");
			status = 1;
			goto done;
		}
		sock = socket_path;
	}
	if (bgpd_daemon_listen(daemon, sock) < 0) {
		fprintf(stderr, "Socket %s: %s\n", sock, strerror(errno));
		status = 1;
		goto done;
	}
	if (audio_sock == NULL) {
		if (bgpd_streams_default_path(BGPD_STREAM_PLAYBACK, audio_path,
					      sizeof(audio_path)) < 0) {
			fprintf(stderr, "No XDG_RUNTIME_DIR for the socket\n");
			status = 1;
			goto done;
		}
		audio_sock = audio_path;
	}
	if (capture_sock == NULL) {
		if (bgpd_streams_default_path(BGPD_STREAM_CAPTURE, capture_path,
					      sizeof(capture_path)) < 0) {
			fprintf(stderr, "No XDG_RUNTIME_DIR for the socket\n");
			status = 1;
			goto done;
		}
		capture_sock = capture_path;
	}
	if (bgpd_daemon_listen_streams(daemon, audio_sock, capture_sock) < 0) {
		fprintf(stderr, "Sockets %s and %s: %s\n", audio_sock,
			capture_sock, strerror(errno));
		status = 1;
		goto done;
	}
	run(daemon);
done:
	bgpd_daemon_close(daemon);
	bgpd_mgmt_close(m);
	free(daemon);
	free(m);
	return status;
}
