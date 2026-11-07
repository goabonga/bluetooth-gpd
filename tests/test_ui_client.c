/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * The interface as a client of the daemon: a fake daemon in the test sends
 * a state, keys are fed to the interface and the commands that reach the
 * daemon are checked. The screen is drawn on /dev/null.
 */
#include "ui.h"

#include <ctui/theme.h>
#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static const uint8_t keyboard[BGPD_ADDR_SIZE] = {
    1, 2, 3, 4, 5, 6, BGPD_ADDR_LE_PUBLIC};
static const uint8_t headset[BGPD_ADDR_SIZE] = {
    9, 8, 7, 6, 5, 4, BGPD_ADDR_BREDR};

static struct bgpd_ipc_server server_storage;
static struct bgpd_mgmt daemon_storage; /* what the fake daemon knows */
static struct bgpd_ipc_server *server = &server_storage;
static struct bgpd_mgmt *daemon_state = &daemon_storage;
static struct bgpd_ipc_client client_storage;
static struct bgpd_mgmt mirror_storage;
static struct bgpd_ui ui_storage;
static struct bgpd_ipc_command received[16];
static size_t received_count;

static void handler(void *context, const struct bgpd_ipc_command *command)
{
	(void)context;
	if (received_count < sizeof(received) / sizeof(received[0]))
		received[received_count++] = *command;
}

/* Lets both sides exchange what they have. */
static void exchange(struct bgpd_ui *ui)
{
	for (int i = 0; i < 4; i++) {
		bgpd_ipc_serve(server, daemon_state, handler, NULL);
		bgpd_ui_poll(ui);
		usleep(5000);
	}
	bgpd_ui_draw(ui);
}

/* Types a key and returns the command it sent, or NULL. */
static const struct bgpd_ipc_command *press(struct bgpd_ui *ui, int key)
{
	size_t before = received_count;

	CHECK(bgpd_ui_key(ui, key));
	exchange(ui);
	return received_count > before ? &received[received_count - 1] : NULL;
}

static void open_prompt(struct bgpd_ui *ui, uint16_t kind, const char *text)
{
	daemon_state->prompt_kind = kind;
	snprintf(daemon_state->prompt, sizeof(daemon_state->prompt), "%s",
		 text);
	exchange(ui);
}

static void close_prompt(struct bgpd_ui *ui)
{
	daemon_state->prompt_kind = BGPD_PROMPT_NONE;
	daemon_state->prompt[0] = '\0';
	exchange(ui);
}

int main(void)
{
	struct bgpd_ipc_client *client = &client_storage;
	struct bgpd_mgmt *mirror = &mirror_storage;
	struct bgpd_ui *ui = &ui_storage;
	char dir[] = "/tmp/bgpd-ui-XXXXXX";
	char path[128];
	const struct bgpd_ipc_command *c;
	struct bgpd_device *d;
	FILE *out, *in;
	SCREEN *screen;

	if (mkdtemp(dir) == NULL)
		return 1;
	snprintf(path, sizeof(path), "%s/bluetooth.sock", dir);
	setenv("TERM", "xterm", 1);
	out = fopen("/dev/null", "w");
	in = fopen("/dev/null", "r");
	screen = newterm("xterm", out, in);
	if (out == NULL || in == NULL || screen == NULL) {
		if (in != NULL)
			fclose(in);
		if (out != NULL)
			fclose(out);
		return 1;
	}
	set_term(screen);
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	ctui_theme_init();

	bgpd_mgmt_init(daemon_state, BGPD_NO_INDEX, NULL);
	daemon_state->ready = true;
	daemon_state->settings = 1U;
	d = bgpd_devices_get(&daemon_state->devices, keyboard);
	if (d != NULL) {
		snprintf(d->name, sizeof(d->name), "Keyboard");
		d->paired = true;
	}
	d = bgpd_devices_get(&daemon_state->devices, headset);
	if (d != NULL)
		snprintf(d->name, sizeof(d->name), "Headset");

	CHECK(bgpd_ipc_listen(server, path) == 0);
	bgpd_mgmt_init(mirror, BGPD_NO_INDEX, NULL);
	bgpd_ipc_connect(client, path);
	bgpd_ui_init(ui, mirror, false);
	bgpd_ui_attach(ui, client);
	exchange(ui);
	CHECK(mirror->ready && mirror->devices.count == 2);
	CHECK(ui->visible == 2);

	/* Each key becomes a command of the daemon. */
	c = press(ui, 's');
	CHECK(c != NULL && c->kind == BGPD_IPC_SCAN);
	c = press(ui, 'o');
	CHECK(c != NULL && c->kind == BGPD_IPC_POWER);
	c = press(ui, 'a');
	CHECK(c != NULL && c->kind == BGPD_IPC_ADAPTER);
	c = press(ui, 'v');
	CHECK(c != NULL && c->kind == BGPD_IPC_VISIBLE);

	/* The first row is the keyboard. */
	/* The volume keys need an audio device. */
	CHECK(press(ui, '+') == NULL);
	CHECK(strstr(mirror->status, "No audio device") != NULL);
	daemon_state->audio.active = true;
	memcpy(daemon_state->audio.addr, headset, BGPD_ADDR_SIZE);
	snprintf(daemon_state->audio.name, sizeof(daemon_state->audio.name),
		 "Headset");
	daemon_state->audio.volume = 40;
	exchange(ui);
	c = press(ui, '+');
	CHECK(c != NULL && c->kind == BGPD_IPC_VOLUME && c->value == 45);
	c = press(ui, '-');
	CHECK(c != NULL && c->kind == BGPD_IPC_VOLUME && c->value == 35);
	daemon_state->audio.volume = 98;
	exchange(ui);
	c = press(ui, '+');
	CHECK(c != NULL && c->value == 100);
	daemon_state->audio.active = false;
	exchange(ui);

	c = press(ui, 'p');
	CHECK(c != NULL && c->kind == BGPD_IPC_PAIR);
	CHECK(c != NULL && memcmp(c->addr, keyboard, BGPD_ADDR_SIZE) == 0);
	c = press(ui, 'c');
	CHECK(c != NULL && c->kind == BGPD_IPC_CONNECT);
	c = press(ui, 'd');
	CHECK(c != NULL && c->kind == BGPD_IPC_DISCONNECT);

	/* Forgetting asks first. */
	CHECK(press(ui, 'x') == NULL);
	c = press(ui, 'y');
	CHECK(c != NULL && c->kind == BGPD_IPC_UNPAIR);
	CHECK(c != NULL && memcmp(c->addr, keyboard, BGPD_ADDR_SIZE) == 0);

	/* The second row is the headset. */
	CHECK(press(ui, KEY_DOWN) == NULL);
	c = press(ui, 'p');
	CHECK(c != NULL && memcmp(c->addr, headset, BGPD_ADDR_SIZE) == 0);
	c = press(ui, 't');
	CHECK(c != NULL && c->kind == BGPD_IPC_TONE);
	CHECK(c != NULL && memcmp(c->addr, headset, BGPD_ADDR_SIZE) == 0);

	/* The PIN typed in the dialog goes to the daemon, once. */
	open_prompt(ui, BGPD_PROMPT_PIN, "PIN for Keyboard?");
	for (const char *k = "1234"; *k != '\0'; k++)
		CHECK(press(ui, *k) == NULL);
	c = press(ui, '\n');
	CHECK(c != NULL && c->kind == BGPD_IPC_ACCEPT);
	CHECK(c != NULL && strcmp(c->input, "1234") == 0);
	/* Answered: the dialog is gone before the daemon says so. */
	received_count = 0;
	CHECK(!bgpd_ui_key(ui, 'q'));
	CHECK(received_count == 0);
	close_prompt(ui);

	/* A confirmation can be rejected. */
	open_prompt(ui, BGPD_PROMPT_CONFIRM, "Pair with Keyboard?");
	c = press(ui, 'n');
	CHECK(c != NULL && c->kind == BGPD_IPC_REJECT);
	close_prompt(ui);

	/* A pairing in progress is cancelled with Escape. */
	daemon_state->pairing = true;
	exchange(ui);
	c = press(ui, 27);
	CHECK(c != NULL && c->kind == BGPD_IPC_CANCEL);

	/* Without the daemon, operations say so. */
	bgpd_ipc_server_close(server);
	exchange(ui);
	CHECK(!mirror->ready);
	CHECK(strstr(mirror->status, "daemon") != NULL);
	CHECK(bgpd_ui_key(ui, 's'));

	bgpd_ipc_client_close(client);
	endwin();
	delscreen(screen);
	fclose(in);
	fclose(out);
	rmdir(dir);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
