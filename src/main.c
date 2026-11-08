/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "bgpd.h"
#include "ipc.h"
#include "mgmt.h"
#include "ui.h"

#include <ctui/theme.h>
#include <curses.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USAGE                                                                  \
	"Usage: bluetooth [--demo] [--ascii] [--socket PATH]\n"                \
	"Manages Bluetooth devices through bluetooth-daemon, which must "      \
	"run.\n"

static volatile sig_atomic_t quitting;

static void interrupted(int sig)
{
	(void)sig;
	quitting = 1;
}

static void run(struct bgpd_ui *ui)
{
	timeout(50);
	while (!quitting) {
		int key;

		bgpd_ui_poll(ui);
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
	struct bgpd_ipc_client *client = calloc(1, sizeof(*client));
	char socket_path[108];
	const char *sock = NULL;
	bool demo = false;
	int status = 0;

	if (m == NULL || ui == NULL || client == NULL) {
		status = 1;
		goto done;
	}
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	client->fd = -1;
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
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			sock = argv[++i];
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
	if (demo) {
		bgpd_ui_demo(m);
	} else {
		if (sock == NULL) {
			if (bgpd_ipc_default_path(socket_path,
						  sizeof(socket_path)) < 0) {
				fputs("The daemon socket path is too long.\n",
				      stderr);
				status = 1;
				goto done;
			}
			sock = socket_path;
		}
		bgpd_ipc_connect(client, sock);
	}
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
	if (!demo)
		bgpd_ui_attach(ui, client);
	run(ui);
	endwin();
done:
	if (client != NULL)
		bgpd_ipc_client_close(client);
	free(client);
	free(ui);
	free(m);
	return status;
}
