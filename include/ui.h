/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * ui.h - the terminal interface, drawn with ctui.
 *
 * It shows the devices of a bgpd_mgmt, turns keys into operations on it and
 * opens ctui dialogs for the authentication prompts and for forgetting a
 * device. In demo mode the operations act on the device table only, so
 * that the interface can be tried without hardware or privileges.
 */
#ifndef BGPD_UI_H
#define BGPD_UI_H

#include <ctui/dialog.h>
#include <ctui/table.h>

#include "hog.h"
#include "ipc.h"
#include "mgmt.h"

#define BGPD_UI_MIN_COLUMNS 60
#define BGPD_UI_MIN_LINES 12

struct bgpd_ui {
	struct bgpd_mgmt *mgmt;
	bool demo;
	bool paired_only;    /* the "Paired" tab */
	bool was_pairing;    /* a pairing was in progress at the last poll */
	struct bgpd_hog hog; /* the BLE HID device being served */
	struct bgpd_ipc_client *client; /* the daemon, or NULL */
	bool answered; /* the open prompt was answered, the daemon is told */
	size_t rows[BGPD_MAX_DEVICES];
	size_t visible;
	ctui_table table;
	char cell[BGPD_NAME_SIZE]; /* text of the cell being drawn */
	/* Dialogs: at most one is open. */
	bool forgetting;
	ctui_confirm forget;
	char forget_label[BGPD_NAME_SIZE + 32];
	uint8_t forget_addr[BGPD_ADDR_SIZE];
	uint16_t shown_prompt; /* prompt_kind the dialog below was made for */
	ctui_confirm confirm;
	ctui_prompt entry;
};

/* Fills the device table with three fake devices and marks m as ready. */
void bgpd_ui_demo(struct bgpd_mgmt *m);

/* ctui and curses must be initialised. */
void bgpd_ui_init(struct bgpd_ui *ui, struct bgpd_mgmt *m, bool demo);

/*
 * Makes the interface a client of the daemon: operations are sent to it and
 * mgmt, kept up to date from its snapshots, is only a copy of its state.
 */
void bgpd_ui_attach(struct bgpd_ui *ui, struct bgpd_ipc_client *client);

/* Advances the BLE connection and follows the pairing; call often. */
void bgpd_ui_poll(struct bgpd_ui *ui);

/* Redraws the whole screen. */
void bgpd_ui_draw(struct bgpd_ui *ui);

/* Handles a key; false when the user quits. */
bool bgpd_ui_key(struct bgpd_ui *ui, int key);

#endif /* BGPD_UI_H */
