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
	"[--trace FILE]\n"                                                     \
	"Keeps the paired Bluetooth LE devices connected through the Linux "   \
	"kernel.\n"

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
	char last[BGPD_STATUS_SIZE] = "";

	while (!quitting) {
		bgpd_daemon_poll(daemon);
		if (strcmp(last, daemon->mgmt->status) != 0) {
			snprintf(last, sizeof(last), "%s",
				 daemon->mgmt->status);
			fprintf(stderr, "%s\n", last);
		}
		/* Wakes on kernel events, and often enough for the HID links.
		 */
		if (poll(&p, 1, 50) < 0 && errno != EINTR)
			break;
	}
}

int main(int argc, char **argv)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_daemon *daemon = calloc(1, sizeof(*daemon));
	char state_dir[4096];
	uint16_t adapter = BGPD_NO_INDEX;
	const char *dir = NULL, *trace = NULL;
	int status = 0;

	if (m == NULL || daemon == NULL) {
		free(daemon);
		free(m);
		return 1;
	}
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
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
	bgpd_daemon_init(daemon, m, stderr);
	run(daemon);
	bgpd_daemon_close(daemon);
done:
	bgpd_mgmt_close(m);
	free(daemon);
	free(m);
	return status;
}
