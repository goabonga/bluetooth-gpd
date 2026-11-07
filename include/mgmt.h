/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * mgmt.h - client of the Linux kernel Bluetooth Management API.
 *
 * Talks to the kernel through a raw HCI control socket: no BlueZ service,
 * libbluetooth or D-Bus. The client is a state machine driven by two
 * calls: operations send one command (only one is in flight at a time) and
 * bgpd_mgmt_poll() reads the answers and events without blocking. The
 * results are read from the public fields.
 */
#ifndef BGPD_MGMT_H
#define BGPD_MGMT_H

#include "device.h"
#include "store.h"

#define BGPD_MAX_CONTROLLERS 64
#define BGPD_NO_INDEX 0xffff

/* Management API opcodes. */
enum bgpd_mgmt_op {
	BGPD_OP_INDEX_LIST = 0x0003,
	BGPD_OP_INFO = 0x0004,
	BGPD_OP_POWER = 0x0005,
	BGPD_OP_BONDABLE = 0x0009,
	BGPD_OP_SSP = 0x000b,
	BGPD_OP_LE = 0x000d,
	BGPD_OP_LOAD_LINK_KEYS = 0x0012,
	BGPD_OP_LOAD_LTKS = 0x0013,
	BGPD_OP_DISCONNECT = 0x0014,
	BGPD_OP_CONNECTIONS = 0x0015,
	BGPD_OP_PIN_REPLY = 0x0016,
	BGPD_OP_PIN_NEGATIVE = 0x0017,
	BGPD_OP_PAIR = 0x0019,
	BGPD_OP_CANCEL_PAIR = 0x001a,
	BGPD_OP_UNPAIR = 0x001b,
	BGPD_OP_CONFIRM = 0x001c,
	BGPD_OP_CONFIRM_NEGATIVE = 0x001d,
	BGPD_OP_PASSKEY = 0x001e,
	BGPD_OP_PASSKEY_NEGATIVE = 0x001f,
	BGPD_OP_START_DISCOVERY = 0x0023,
	BGPD_OP_STOP_DISCOVERY = 0x0024,
	BGPD_OP_CONFIRM_NAME = 0x0025,
	BGPD_OP_LOAD_IRKS = 0x0030,
	BGPD_OP_BREDR = 0x002a,
};

/* Events that open an authentication prompt (see prompt_kind). */
enum bgpd_prompt {
	BGPD_PROMPT_NONE = 0,
	BGPD_PROMPT_PIN = 0x0e,
	BGPD_PROMPT_CONFIRM = 0x0f,
	BGPD_PROMPT_PASSKEY = 0x10,
};

#define BGPD_STATUS_SIZE 512
#define BGPD_PROMPT_SIZE 256
#define BGPD_INPUT_SIZE 17

struct bgpd_mgmt {
	int fd;
	uint16_t index, requested_index;
	uint16_t controllers[BGPD_MAX_CONTROLLERS];
	size_t controller_count;
	uint32_t supported, settings;
	uint16_t command, prompt_kind, prepare_action;
	uint8_t scan_type;
	uint8_t pair_addr[BGPD_ADDR_SIZE], pair_original[BGPD_ADDR_SIZE];
	uint8_t prompt_addr[BGPD_ADDR_SIZE], unpair_addr[BGPD_ADDR_SIZE];
	bool ready, scanning, own_scan, pairing, secure_pin, storage_error;
	uint64_t deadline;
	char controller_name[BGPD_NAME_SIZE];
	char controller_address[BGPD_ADDR_TEXT];
	char status[BGPD_STATUS_SIZE]; /* last message, for the user */
	char prompt[BGPD_PROMPT_SIZE]; /* question of the open prompt */
	char input[BGPD_INPUT_SIZE];   /* PIN or passkey being typed */
	const char *state_dir;
	struct bgpd_devices devices;
	struct bgpd_store store;
};

/* Little-endian helpers of the protocol. */
uint16_t bgpd_get16(const uint8_t *p);
uint32_t bgpd_get32(const uint8_t *p);
void bgpd_put16(uint8_t *p, uint16_t n);
void bgpd_put32(uint8_t *p, uint32_t n);

/* Milliseconds on the monotonic clock. */
uint64_t bgpd_now(void);

/* Empties m; requested_index is the adapter to use or BGPD_NO_INDEX. */
void bgpd_mgmt_init(struct bgpd_mgmt *m, uint16_t requested_index,
		    const char *state_dir);

/*
 * Drops every capability of the process and forbids new privileges. The
 * management socket keeps the authorisation it got when it was bound.
 */
int bgpd_drop_capabilities(void);

/*
 * Opens the control socket (needs CAP_NET_ADMIN), drops the capabilities
 * and asks for the adapter list. On failure m->status says why.
 */
int bgpd_mgmt_open(struct bgpd_mgmt *m);

/* Uses an already open socket instead (tests) and asks for the adapters. */
int bgpd_mgmt_attach(struct bgpd_mgmt *m, int fd);

/* Reads and handles what the kernel sent; expires a stuck command. */
void bgpd_mgmt_poll(struct bgpd_mgmt *m);

/* Handles one packet read from the socket. */
void bgpd_mgmt_packet(struct bgpd_mgmt *m, const uint8_t *packet, size_t size);

/* Starts or stops scanning for Classic and LE devices. */
void bgpd_mgmt_scan(struct bgpd_mgmt *m);

/* Turns the adapter on or off. */
void bgpd_mgmt_toggle_power(struct bgpd_mgmt *m);

/*
 * Pairs with a device. The kernel then asks for a PIN, a passkey or a
 * confirmation: prompt_kind, prompt and input describe the open prompt and
 * bgpd_mgmt_reply() answers it. Unsolicited requests are refused.
 */
void bgpd_mgmt_pair(struct bgpd_mgmt *m, const struct bgpd_device *device);

/* Cancels the pairing in progress. */
void bgpd_mgmt_cancel_pair(struct bgpd_mgmt *m);

/* Accepts the prompt (with input for a PIN or passkey) or rejects it. */
void bgpd_mgmt_reply(struct bgpd_mgmt *m, bool accept);

/* Removes the pairing of a device and disconnects it. */
void bgpd_mgmt_unpair(struct bgpd_mgmt *m, const struct bgpd_device *device);

/* Disconnects a device. */
void bgpd_mgmt_disconnect(struct bgpd_mgmt *m,
			  const struct bgpd_device *device);

/* Switches to the next adapter, or refreshes the list when not ready. */
void bgpd_mgmt_next_adapter(struct bgpd_mgmt *m);

/* Stops what is running and closes the socket and the key store. */
void bgpd_mgmt_close(struct bgpd_mgmt *m);

#endif /* BGPD_MGMT_H */
