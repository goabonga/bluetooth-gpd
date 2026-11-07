/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * Runs the program in demo mode in a pseudo-terminal, types keys and checks
 * the screen output. Usage: test_ui PROGRAM
 */
#include "bgpd.h"
#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                              \
			failures++;                                            \
		}                                                              \
	} while (0)

static int master;
static pid_t child;
static char screen[1U << 18U];
static size_t used;

/* Reads whatever the program printed within ms milliseconds. */
static void drain(int ms)
{
	struct pollfd p = {.fd = master, .events = POLLIN};

	while (poll(&p, 1, ms) == 1) {
		ssize_t n =
		    read(master, screen + used, sizeof(screen) - used - 1);

		if (n <= 0)
			break;
		used += (size_t)n;
		screen[used] = '\0';
		ms = 50;
	}
}

/* Called while waiting, to run the fake daemon of a test. */
static void (*pump)(void);

static bool seen(const char *text)
{
	for (int i = 0; i < 40; i++) {
		if (strstr(screen, text) != NULL)
			return true;
		if (pump != NULL)
			pump();
		drain(50);
	}
	return strstr(screen, text) != NULL;
}

static void type(const char *keys)
{
	CHECK(write(master, keys, strlen(keys)) == (ssize_t)strlen(keys));
	for (int i = 0; i < 3; i++) {
		if (pump != NULL)
			pump();
		drain(50);
	}
}

static void start(const char *program, char *const args[])
{
	struct winsize size = {.ws_row = 24, .ws_col = 100};

	used = 0;
	screen[0] = '\0';
	(void)args;
	child = forkpty(&master, NULL, NULL, &size);
	if (child == 0) {
		char *const argv[] = {(char *)program, args[0], args[1], NULL};

		setenv("TERM", "xterm-256color", 1);
		/* The program is the one the Makefile passes. */
		/* NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint) */
		execv(program, argv);
		_exit(127);
	}
}

static int finish(void)
{
	int status = 0;

	drain(200);
	if (waitpid(child, &status, WNOHANG) == 0) {
		kill(child, SIGTERM);
		waitpid(child, &status, 0);
	}
	close(master);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_options(const char *program)
{
	char *version[] = {"--version", NULL};
	char *bad[] = {"--bogus", NULL};

	start(program, version);
	CHECK(seen(BGPD_VERSION));
	CHECK(finish() == 0);
	start(program, bad);
	CHECK(seen("Unknown or incomplete argument"));
	CHECK(finish() == 2);
}

static void test_demo(const char *program)
{
	char *args[] = {"--demo", "--ascii"};

	start(program, args);
	CHECK(seen("Audio headset"));
	CHECK(seen("DEMO: no hardware access"));
	type("s");
	CHECK(seen("active"));
	type("v");
	CHECK(seen("yes"));
	type("j");
	type("p");
	CHECK(seen("simulated pairing"));
	type("c");
	CHECK(seen("connection"));
	type("\t");
	CHECK(seen("Paired"));
	type("x");
	CHECK(seen("Forget Keyboard"));
	type("y");
	type("o");
	CHECK(seen(" OFF "));
	type("q");
	CHECK(finish() == 0);
}

static struct bgpd_ipc_server server;
static struct bgpd_mgmt daemon_state;
static struct bgpd_ipc_command received[8];
static size_t received_count;

static void handler(void *context, const struct bgpd_ipc_command *command)
{
	(void)context;
	if (received_count < sizeof(received) / sizeof(received[0]))
		received[received_count++] = *command;
}

static void serve(void)
{
	bgpd_ipc_serve(&server, &daemon_state, handler, NULL);
}

/* The program as a client of a daemon played by the test. */
static void test_client(const char *program)
{
	static const uint8_t keyboard[BGPD_ADDR_SIZE] = {1, 2, 3, 4, 5, 6, 1};
	char dir[] = "/tmp/bgpd-ui-XXXXXX";
	char path[128];
	char *args[] = {"--socket", path};
	struct bgpd_device *d;

	if (mkdtemp(dir) == NULL) {
		CHECK(!"temporary directory");
		return;
	}
	snprintf(path, sizeof(path), "%s/bluetooth.sock", dir);
	bgpd_mgmt_init(&daemon_state, BGPD_NO_INDEX, NULL);
	daemon_state.ready = true;
	daemon_state.settings = 1U;
	d = bgpd_devices_get(&daemon_state.devices, keyboard);
	if (d != NULL)
		snprintf(d->name, sizeof(d->name), "Flipper keyboard");
	CHECK(bgpd_ipc_listen(&server, path) == 0);
	received_count = 0;
	pump = serve;

	start(program, args);
	CHECK(seen("Flipper keyboard"));
	type("s");
	CHECK(received_count == 1 && received[0].kind == BGPD_IPC_SCAN);

	daemon_state.prompt_kind = BGPD_PROMPT_PIN;
	snprintf(daemon_state.prompt, sizeof(daemon_state.prompt),
		 "Enter the PIN of the keyboard");
	CHECK(seen("Enter the PIN of the keyboard"));
	type("1234\n");
	CHECK(received_count == 2 && received[1].kind == BGPD_IPC_ACCEPT);
	CHECK(strcmp(received[1].input, "1234") == 0);

	bgpd_ipc_server_close(&server);
	CHECK(seen("Waiting for the daemon"));
	type("q");
	CHECK(finish() == 0);
	pump = NULL;
	rmdir(dir);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fputs("usage: test_ui PROGRAM\n", stderr);
		return 2;
	}
	test_options(argv[1]);
	test_demo(argv[1]);
	test_client(argv[1]);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
