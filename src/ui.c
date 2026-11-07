/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ui.h"

#include <ctui/statusbar.h>
#include <ctui/tabbar.h>
#include <ctui/text.h>
#include <ctui/theme.h>
#include <curses.h>
#include <stdio.h>
#include <string.h>

enum column { COL_NAME, COL_ADDRESS, COL_TYPE, COL_FLAGS, COL_RSSI, COLS_ };

static const ctui_table_column columns[COLS_] = {
    [COL_NAME] = {.title = "Name", .width = 0, .flex = 1},
    [COL_ADDRESS] = {.title = "Address", .width = 17},
    [COL_TYPE] = {.title = "Type", .width = 4},
    [COL_FLAGS] = {.title = "P C S", .width = 5},
    [COL_RSSI] = {.title = "RSSI", .width = 4, .align = CTUI_TABLE_RIGHT},
};

static const char *const hints =
    "s scan  v visible  p pair  c connect  t tone  d disconnect  x forget  "
    "o power  a adapter  Tab filter  Esc cancel  q quit";

static void say(struct bgpd_mgmt *m, const char *text)
{
	snprintf(m->status, sizeof(m->status), "%s", text);
}

/* Sends an operation to the daemon. */
static void send_command(struct bgpd_ui *ui, enum bgpd_ipc_kind kind,
			 const struct bgpd_device *d)
{
	struct bgpd_ipc_command command = {.kind = kind};

	if (d != NULL)
		memcpy(command.addr, d->addr, BGPD_ADDR_SIZE);
	if (kind == BGPD_IPC_ACCEPT)
		snprintf(command.input, sizeof(command.input), "%s",
			 ui->mgmt->input);
	if (bgpd_ipc_send(ui->client, &command) < 0)
		say(ui->mgmt, "The daemon is not reachable");
}

/* The prompt that waits for an answer, or none. */
static uint16_t open_prompt(const struct bgpd_ui *ui)
{
	return ui->answered ? BGPD_PROMPT_NONE : ui->mgmt->prompt_kind;
}

static void reply(struct bgpd_ui *ui, bool accept)
{
	if (ui->client == NULL)
		return;
	send_command(ui, accept ? BGPD_IPC_ACCEPT : BGPD_IPC_REJECT, NULL);
	ui->answered = true;
}

static struct bgpd_device *selected_device(struct bgpd_ui *ui)
{
	if (ui->visible == 0)
		return NULL;
	return &ui->mgmt->devices.items[ui->rows[ui->table.selected]];
}

static void refresh_rows(struct bgpd_ui *ui)
{
	const struct bgpd_devices *devices = &ui->mgmt->devices;

	ui->visible = 0;
	for (size_t i = 0; i < devices->count; i++)
		if (!ui->paired_only || devices->items[i].paired)
			ui->rows[ui->visible++] = i;
	ctui_table_set_rows(&ui->table, ui->visible);
}

static const char *cell(void *ctx, size_t row, size_t col)
{
	struct bgpd_ui *ui = ctx;
	const struct bgpd_device *d = &ui->mgmt->devices.items[ui->rows[row]];

	switch (col) {
	case COL_NAME:
		return d->name;
	case COL_ADDRESS:
		bgpd_address_format(d->addr, ui->cell);
		return ui->cell;
	case COL_TYPE:
		return bgpd_address_type_name(d->addr[6]);
	case COL_FLAGS:
		snprintf(ui->cell, sizeof(ui->cell), "%c %c %c",
			 d->paired ? 'P' : '-', d->connected ? 'C' : '-',
			 d->saved ? 'S' : '-');
		return ui->cell;
	default:
		if (!d->has_rssi)
			return "--";
		snprintf(ui->cell, sizeof(ui->cell), "%d", d->rssi);
		return ui->cell;
	}
}

void bgpd_ui_demo(struct bgpd_mgmt *m)
{
	static const char *const names[] = {"Audio headset", "Keyboard",
					    "OBD II"};

	m->ready = true;
	m->index = 0;
	m->settings = 1U | (1U << 7U) | (1U << 9U);
	snprintf(m->controller_name, sizeof(m->controller_name),
		 "Demonstration");
	for (uint8_t i = 0; i < 3; i++) {
		const uint8_t addr[BGPD_ADDR_SIZE] = {i,    0x44, 0x33, 0x22,
						      0x11, 0x00, 0x00};
		struct bgpd_device *d = bgpd_devices_get(&m->devices, addr);

		snprintf(d->name, sizeof(d->name), "%s", names[i]);
		d->has_rssi = true;
		d->rssi = -40 - 15 * i;
	}
	snprintf(m->status, sizeof(m->status), "DEMO: no hardware access");
}

void bgpd_ui_init(struct bgpd_ui *ui, struct bgpd_mgmt *m, bool demo)
{
	memset(ui, 0, sizeof(*ui));
	ui->mgmt = m;
	ui->demo = demo;
	ctui_table_init(&ui->table, columns, COLS_, 0, cell, ui);
}

static void connect_device(struct bgpd_ui *ui, struct bgpd_device *d)
{
	if (ui->client != NULL) {
		send_command(ui, BGPD_IPC_CONNECT, d);
		return;
	}
	d->connected = true;
	say(ui->mgmt, "DEMO: simulated connection");
}

void bgpd_ui_attach(struct bgpd_ui *ui, struct bgpd_ipc_client *client)
{
	ui->client = client;
}

void bgpd_ui_poll(struct bgpd_ui *ui)
{
	if (ui->client != NULL)
		bgpd_ipc_poll(ui->client, ui->mgmt);
}

static ctui_tone state_tone(const struct bgpd_mgmt *m, const char **text)
{
	if ((m->settings & 1U) == 0) {
		*text = " OFF ";
		return CTUI_TONE_BAD;
	}
	if (m->command != 0 || m->pairing || m->scanning) {
		*text = " BUSY ";
		return CTUI_TONE_BUSY;
	}
	*text = " ON ";
	return CTUI_TONE_OK;
}

static void draw_bars(struct bgpd_ui *ui)
{
	const struct bgpd_mgmt *m = ui->mgmt;
	static const ctui_tab tabs[] = {{.label = "All", .enabled = true},
					{.label = "Paired", .enabled = true}};
	ctui_tabbar bar = {.brand = "BLUETOOTH GPD",
			   .tabs = tabs,
			   .count = 2,
			   .current = ui->paired_only ? 1 : 0};
	char adapter[32], count[32];
	const char *state;
	ctui_tone tone = state_tone(m, &state);
	ctui_segment left[] = {{state, tone, true},
			       {adapter, CTUI_TONE_BAR, false}};
	ctui_segment right[] = {{count, CTUI_TONE_DARK, false}};

	snprintf(adapter, sizeof(adapter), " hci%u ", m->index);
	snprintf(count, sizeof(count), " %zu devices ", ui->visible);
	ctui_tabbar_draw(0, &bar);
	ctui_statusbar_draw(LINES - 1, left, 2, right, 1);
}

/* Creates the dialog of a prompt that has just appeared. */
static void sync_dialog(struct bgpd_ui *ui)
{
	struct bgpd_mgmt *m = ui->mgmt;

	if (m->prompt_kind == ui->shown_prompt)
		return;
	ui->shown_prompt = m->prompt_kind;
	ui->answered = false;
	if (m->prompt_kind == BGPD_PROMPT_CONFIRM)
		ctui_confirm_init(&ui->confirm, m->prompt, NULL);
	else if (m->prompt_kind != BGPD_PROMPT_NONE)
		ctui_prompt_init(&ui->entry, m->prompt, m->input,
				 sizeof(m->input));
}

static void draw_dialog(struct bgpd_ui *ui)
{
	bool typing;

	sync_dialog(ui);
	typing = !ui->forgetting && open_prompt(ui) != BGPD_PROMPT_NONE &&
		 open_prompt(ui) != BGPD_PROMPT_CONFIRM;
	curs_set(typing ? 1 : 0);
	if (ui->forgetting)
		ctui_confirm_draw(&ui->forget);
	else if (open_prompt(ui) == BGPD_PROMPT_CONFIRM)
		ctui_confirm_draw(&ui->confirm);
	else if (typing)
		ctui_prompt_draw(&ui->entry);
}

void bgpd_ui_draw(struct bgpd_ui *ui)
{
	const struct bgpd_mgmt *m = ui->mgmt;
	int body = ctui_attr(CTUI_PAIR_BODY, false);
	char line[BGPD_STATUS_SIZE];

	erase();
	if (LINES < BGPD_UI_MIN_LINES || COLS < BGPD_UI_MIN_COLUMNS) {
		ctui_put(0, 0, COLS,
			 "Terminal too small: minimum 60 x 12. "
			 "q quits.",
			 body);
		refresh();
		return;
	}
	refresh_rows(ui);
	draw_bars(ui);
	snprintf(line, sizeof(line),
		 "hci%u  %.24s  |  Power %s  |  Scan %s  |  Visible %s",
		 m->index, m->controller_name,
		 (m->settings & 1U) != 0 ? "on" : "off",
		 m->scanning ? "active" : "stopped",
		 (m->settings & BGPD_SETTING_DISCOVERABLE) != 0 ? "yes" : "no");
	ctui_put(2, 2, COLS - 4, line, ctui_attr(CTUI_PAIR_HEADING, true));
	if (ui->visible == 0)
		ctui_put(5, 2, COLS - 4,
			 "No devices. s starts scanning; o turns the adapter "
			 "on.",
			 ctui_attr(CTUI_PAIR_MUTED, false));
	else
		ctui_table_draw(&ui->table, 4, 1, COLS - 2, LINES - 8);
	if (m->warning[0] != '\0')
		ctui_put(LINES - 4, 1, COLS - 2, m->warning,
			 ctui_attr(CTUI_PAIR_ERROR, true));
	ctui_put(LINES - 3, 1, COLS - 2, m->status, body);
	ctui_put(LINES - 2, 1, COLS - 2, hints,
		 ctui_attr(CTUI_PAIR_MUTED, false));
	draw_dialog(ui);
	refresh();
}

/* ---- operations: sent to the daemon, or played on the table in demo ---- */

static void toggle_power(struct bgpd_ui *ui)
{
	struct bgpd_mgmt *m = ui->mgmt;

	if (ui->client != NULL) {
		send_command(ui, BGPD_IPC_POWER, NULL);
		return;
	}
	m->settings ^= 1U;
	if ((m->settings & 1U) == 0) {
		m->scanning = m->own_scan = false;
		for (size_t i = 0; i < m->devices.count; i++)
			m->devices.items[i].connected = false;
	}
}

static void scan(struct bgpd_ui *ui)
{
	struct bgpd_mgmt *m = ui->mgmt;

	if (ui->client != NULL)
		send_command(ui, BGPD_IPC_SCAN, NULL);
	else if ((m->settings & 1U) == 0)
		say(m, "Turn the adapter on first");
	else
		m->scanning = !m->scanning;
}

static void visible(struct bgpd_ui *ui)
{
	struct bgpd_mgmt *m = ui->mgmt;

	if (ui->client != NULL)
		send_command(ui, BGPD_IPC_VISIBLE, NULL);
	else
		m->settings ^= BGPD_SETTING_DISCOVERABLE;
}

static void pair(struct bgpd_ui *ui, struct bgpd_device *d)
{
	if (ui->client != NULL) {
		send_command(ui, BGPD_IPC_PAIR, d);
		return;
	}
	d->paired = d->saved = true;
	say(ui->mgmt, "DEMO: simulated pairing");
}

static void disconnect(struct bgpd_ui *ui, struct bgpd_device *d)
{
	if (ui->client != NULL)
		send_command(ui, BGPD_IPC_DISCONNECT, d);
	else
		d->connected = false;
}

static void forget(struct bgpd_ui *ui)
{
	struct bgpd_device *d =
	    bgpd_devices_get(&ui->mgmt->devices, ui->forget_addr);

	if (d == NULL)
		return;
	if (ui->client != NULL)
		send_command(ui, BGPD_IPC_UNPAIR, d);
	else
		d->paired = d->saved = d->connected = false;
}

/* ---- dialogs ---- */

static bool dialog_key(struct bgpd_ui *ui, int key)
{
	struct bgpd_mgmt *m = ui->mgmt;
	ctui_dialog_result result;

	if (ui->forgetting) {
		result = ctui_confirm_key(&ui->forget, key);
		if (result == CTUI_DIALOG_OPEN)
			return true;
		ui->forgetting = false;
		if (result == CTUI_DIALOG_ACCEPTED)
			forget(ui);
		return true;
	}
	if (open_prompt(ui) == BGPD_PROMPT_NONE)
		return false;
	if (open_prompt(ui) == BGPD_PROMPT_CONFIRM) {
		result = ctui_confirm_key(&ui->confirm, key);
	} else {
		bool digits = open_prompt(ui) == BGPD_PROMPT_PASSKEY;
		bool character = key >= ' ' && key <= '~';

		if (character && digits &&
		    (key < '0' || key > '9' || ui->entry.used >= 6))
			return true;
		result = ctui_prompt_key(&ui->entry, key);
		m->input[ui->entry.used] = '\0';
	}
	if (result != CTUI_DIALOG_OPEN)
		reply(ui, result == CTUI_DIALOG_ACCEPTED);
	return true;
}

bool bgpd_ui_key(struct bgpd_ui *ui, int key)
{
	struct bgpd_mgmt *m = ui->mgmt;
	struct bgpd_device *d;

	sync_dialog(ui);
	if (dialog_key(ui, key))
		return true;
	if (key == 'q')
		return false;
	if (key == '\t') {
		ui->paired_only = !ui->paired_only;
		ui->table.selected = ui->table.first = 0;
		return true;
	}
	if (key == 27 && m->pairing) {
		if (ui->client != NULL)
			send_command(ui, BGPD_IPC_CANCEL, NULL);
		return true;
	}
	if (key == 'a') {
		if (ui->client != NULL)
			send_command(ui, BGPD_IPC_ADAPTER, NULL);
		else
			say(m, "DEMO: a single adapter");
		return true;
	}
	if (key == 'o' && m->ready)
		toggle_power(ui);
	else if (key == 's' && m->ready)
		scan(ui);
	else if (key == 'v' && m->ready)
		visible(ui);
	if (!m->ready || ui->visible == 0)
		return true;
	ctui_table_key(&ui->table, key, LINES - 8);
	d = selected_device(ui);
	if (d == NULL)
		return true;
	if (key == 'p')
		pair(ui, d);
	else if (key == 'c')
		connect_device(ui, d);
	else if (key == 't') {
		if (ui->client != NULL)
			send_command(ui, BGPD_IPC_TONE, d);
		else
			say(m, "DEMO: no audio");
	} else if (key == 'd')
		disconnect(ui, d);
	else if (key == 'x') {
		memcpy(ui->forget_addr, d->addr, BGPD_ADDR_SIZE);
		snprintf(ui->forget_label, sizeof(ui->forget_label),
			 "Forget %.180s and remove its pairing?", d->name);
		ctui_confirm_init(&ui->forget, ui->forget_label, NULL);
		ui->forgetting = true;
	}
	return true;
}
