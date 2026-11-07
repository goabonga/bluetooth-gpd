/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "bgpd.h"
#include "mgmt.h"
#include "ui.h"

#include <ctui/theme.h>
#include <curses.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USAGE                                                                  \
	"Usage: bluetooth [--demo] [--ascii] [--adapter hci0] "                \
	"[--state-dir DIR] [--trace FILE]\n"                                   \
	"Scans and pairs Bluetooth devices through the Linux kernel; no "      \
	"BlueZ service.\n"

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

static void run(struct bgpd_ui *ui)
{
	struct bgpd_mgmt *m = ui->mgmt;

	timeout(50);
	while (!quitting) {
		int key;

		if (!ui->demo)
			bgpd_mgmt_poll(m);
		bgpd_ui_draw(ui);
		key = getch();
		if (key != ERR && key != KEY_RESIZE && !bgpd_ui_key(ui, key))
			break;
	}
}

int main(int argc, char **argv)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_ui *ui = calloc(1, sizeof(*ui));
	char state_dir[4096];
	uint16_t adapter = BGPD_NO_INDEX;
	const char *dir = NULL, *trace = NULL;
	bool demo = false;
	int status = 0;

	if (m == NULL || ui == NULL) {
		free(ui);
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
		} else if (strcmp(argv[i], "--demo") == 0) {
			demo = true;
		} else if (strcmp(argv[i], "--ascii") == 0) {
			setenv("CTUI_ASCII", "1", 1);
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
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fputs("An interactive terminal is required.\n", stderr);
		status = 1;
		goto done;
	}
	if (dir == NULL && !demo) {
		if (bgpd_store_default_dir(state_dir, sizeof(state_dir)) < 0) {
			fprintf(stderr, "No directory for the keys: %s\n",
				strerror(errno));
			status = 1;
			goto done;
		}
		dir = state_dir;
	}
	bgpd_mgmt_init(m, adapter, dir);
	if (demo) {
		if (bgpd_drop_capabilities() < 0) {
			fprintf(stderr, "Dropping capabilities: %s\n",
				strerror(errno));
			status = 1;
			goto done;
		}
		bgpd_ui_demo(m);
	} else if (trace != NULL && bgpd_mgmt_trace(m, trace) < 0) {
		fprintf(stderr, "Trace file %s: %s\n", trace, strerror(errno));
		status = 1;
		goto done;
	} else if (bgpd_mgmt_open(m) < 0) {
		fprintf(stderr, "%s\n", m->status);
		status = 1;
		goto done;
	}
	if (!demo)
		bgpd_mgmt_check_manager(m, "/proc");
	setlocale(LC_ALL, "");
	signal(SIGINT, interrupted);
	signal(SIGTERM, interrupted);
	signal(SIGPIPE, SIG_IGN);
	set_escdelay(25);
	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	ctui_theme_init();
	bgpd_ui_init(ui, m, demo);
	run(ui);
	endwin();
done:
	bgpd_mgmt_close(m);
	free(ui);
	free(m);
	return status;
}
