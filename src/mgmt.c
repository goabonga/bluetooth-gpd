/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "mgmt.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Kernel values, so that no BlueZ header is needed. */
#define AF_BLUETOOTH_FAMILY 31
#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3

struct sockaddr_hci {
	sa_family_t family;
	uint16_t device, channel;
};

/* Settings and supported settings bits. */
#define SETTING_POWERED (1U << 0U)
#define SETTING_BONDABLE (1U << 4U)
#define SETTING_SSP (1U << 6U)
#define SETTING_BREDR (1U << 7U)
#define SETTING_LE (1U << 9U)

/* Management events. */
#define EV_COMMAND_COMPLETE 0x0001
#define EV_COMMAND_STATUS 0x0002
#define EV_INDEX_ADDED 0x0004
#define EV_INDEX_REMOVED 0x0005
#define EV_NEW_SETTINGS 0x0006
#define EV_NEW_LINK_KEY 0x0009
#define EV_NEW_LTK 0x000a
#define EV_CONNECTED 0x000b
#define EV_DISCONNECTED 0x000c
#define EV_UNPAIRED 0x0016
#define EV_DEVICE_FOUND 0x0012
#define EV_DISCOVERING 0x0013
#define EV_NEW_IRK 0x0018
#define EV_PIN_REQUEST 0x000e
#define EV_USER_CONFIRM 0x000f
#define EV_USER_PASSKEY 0x0010
#define EV_AUTH_FAILED 0x0011
#define EV_PASSKEY_NOTIFY 0x0017

#define PAIR_TIMEOUT_MS 120000
#define IO_CAPABILITY_KEYBOARD_DISPLAY 4
#define SECURE_PIN_LENGTH 16

#define STATUS_OPERATION_NOT_ALLOWED_OFF 0x0f /* adapter is powered off */
#define COMMAND_TIMEOUT_MS 10000
#define INFO_SIZE 280U
#define MAX_PACKET 65541U
#define HEADER_SIZE 6U

uint16_t bgpd_get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((unsigned int)p[1] << 8U));
}

uint32_t bgpd_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
	       ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

void bgpd_put16(uint8_t *p, uint16_t n)
{
	p[0] = (uint8_t)(n & 0xffU);
	p[1] = (uint8_t)(n >> 8U);
}

void bgpd_put32(uint8_t *p, uint32_t n)
{
	for (unsigned int i = 0; i < 4; i++)
		p[i] = (uint8_t)(n >> (8U * i));
}

uint64_t bgpd_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}

static void say(struct bgpd_mgmt *m, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void say(struct bgpd_mgmt *m, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(m->status, sizeof(m->status), format, args);
	va_end(args);
}

void bgpd_mgmt_init(struct bgpd_mgmt *m, uint16_t requested_index,
		    const char *state_dir)
{
	memset(m, 0, sizeof(*m));
	m->fd = -1;
	m->index = BGPD_NO_INDEX;
	m->requested_index = requested_index;
	m->state_dir = state_dir;
	m->store.dirfd = m->store.lockfd = -1;
}

static const char *error_name(uint8_t code)
{
	static const char *const names[] = {
	    "OK",
	    "Unknown command",
	    "Not connected",
	    "Failed",
	    "Connection failed",
	    "Authentication failed",
	    "Not paired",
	    "Insufficient resources",
	    "Timed out",
	    "Already connected",
	    "Busy",
	    "Rejected",
	    "Not supported",
	    "Invalid parameters",
	    "Disconnected",
	    "Adapter powered off",
	    "Cancelled",
	    "Adapter absent",
	    "Blocked by rfkill",
	    "Already paired",
	    "Permission denied",
	};

	return code < sizeof(names) / sizeof(names[0]) ? names[code]
						       : "Kernel error";
}

static bool secret_command(uint16_t op)
{
	return op == BGPD_OP_LOAD_LINK_KEYS || op == BGPD_OP_LOAD_LTKS ||
	       op == BGPD_OP_LOAD_IRKS || op == BGPD_OP_PIN_REPLY ||
	       op == BGPD_OP_PASSKEY;
}

/* Logs one packet; keys, PINs and passkeys are not written. */
static void trace(const struct bgpd_mgmt *m, const char *direction,
		  uint16_t code, uint16_t index, const uint8_t *data,
		  size_t size, bool secret)
{
	if (m->trace == NULL)
		return;
	fprintf(m->trace, "%8llu ms %s 0x%04x hci%u (%zu bytes)",
		(unsigned long long)(bgpd_now() - m->trace_start), direction,
		code, index, size);
	for (size_t i = 0; i < size && !secret; i++)
		fprintf(m->trace, " %02x", data[i]);
	fputs(secret ? " <hidden>\n" : "\n", m->trace);
	fflush(m->trace);
}

int bgpd_mgmt_trace(struct bgpd_mgmt *m, const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0)
		return -1;
	m->trace_start = bgpd_now();
	m->trace = fdopen(fd, "w");
	if (m->trace == NULL) {
		close(fd);
		return -1;
	}
	return 0;
}

static int send_command(struct bgpd_mgmt *m, uint16_t op, uint16_t index,
			const void *data, size_t size)
{
	uint8_t packet[MAX_PACKET];
	ssize_t written;

	if (size > UINT16_MAX) {
		errno = EMSGSIZE;
		return -1;
	}
	bgpd_put16(packet, op);
	bgpd_put16(packet + 2, index);
	bgpd_put16(packet + 4, (uint16_t)size);
	if (size > 0)
		memcpy(packet + HEADER_SIZE, data, size);
	trace(m, ">", op, index, data, size, secret_command(op));
	do {
		written = write(m->fd, packet, size + HEADER_SIZE);
	} while (written < 0 && errno == EINTR);
	if (written != (ssize_t)(size + HEADER_SIZE)) {
		say(m, "MGMT send: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Sends the command and waits for its answer before the next one. */
static int command(struct bgpd_mgmt *m, uint16_t op, const void *data,
		   size_t size)
{
	uint16_t index = op == BGPD_OP_INDEX_LIST ? BGPD_NO_INDEX : m->index;

	if (m->command != 0) {
		say(m, "Operation in progress");
		return -1;
	}
	if (send_command(m, op, index, data, size) < 0)
		return -1;
	m->command = op;
	m->deadline = bgpd_now() + COMMAND_TIMEOUT_MS;
	say(m, "MGMT command 0x%04x in progress...", op);
	return 0;
}

int bgpd_drop_capabilities(void)
{
	struct __user_cap_header_struct header = {
	    .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
	struct __user_cap_data_struct data[2] = {{0, 0, 0}, {0, 0, 0}};

	if (syscall(SYS_capset, &header, data) < 0)
		return -1;
	return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
}

static bool has_net_admin(void)
{
	struct __user_cap_header_struct header = {
	    .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
	struct __user_cap_data_struct data[2] = {{0, 0, 0}, {0, 0, 0}};

	if (syscall(SYS_capget, &header, data) < 0)
		return false;
	return (data[CAP_NET_ADMIN / 32].effective &
		(1U << (CAP_NET_ADMIN % 32U))) != 0;
}

void bgpd_mgmt_check_manager(struct bgpd_mgmt *m, const char *proc)
{
	DIR *dir = opendir(proc);
	struct dirent *entry;

	m->warning[0] = '\0';
	if (dir == NULL)
		return;
	while ((entry = readdir(dir)) != NULL) {
		char path[300], name[32] = {0};
		FILE *f;

		if (!isdigit((unsigned char)entry->d_name[0]))
			continue;
		snprintf(path, sizeof(path), "%s/%.200s/comm", proc,
			 entry->d_name);
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		if (fgets(name, sizeof(name), f) != NULL &&
		    strncmp(name, "bluetoothd", 10) == 0) {
			snprintf(
			    m->warning, sizeof(m->warning),
			    "bluetoothd is running and refuses pairing "
			    "requests: stop it (systemctl stop bluetooth)");
			fclose(f);
			break;
		}
		fclose(f);
	}
	closedir(dir);
}

int bgpd_mgmt_open(struct bgpd_mgmt *m)
{
	struct sockaddr_hci addr = {.family = AF_BLUETOOTH_FAMILY,
				    .device = BGPD_NO_INDEX,
				    .channel = HCI_CHANNEL_CONTROL};
	int fd;

	if (!has_net_admin()) {
		say(m, "Bluetooth access is not configured: the program needs "
		       "the CAP_NET_ADMIN capability");
		return -1;
	}
	fd = socket(AF_BLUETOOTH_FAMILY,
		    SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    bgpd_drop_capabilities() < 0) {
		say(m, "Kernel Bluetooth access: %s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return bgpd_mgmt_attach(m, fd);
}

int bgpd_mgmt_attach(struct bgpd_mgmt *m, int fd)
{
	m->fd = fd;
	return command(m, BGPD_OP_INDEX_LIST, NULL, 0);
}

static void clean_name(char *dst, const uint8_t *src, size_t size)
{
	if (size > BGPD_NAME_SIZE - 1)
		size = BGPD_NAME_SIZE - 1;
	for (size_t i = 0; i < size; i++)
		dst[i] = src[i] >= 32 && src[i] != 127 ? (char)src[i] : ' ';
	dst[size] = '\0';
}

/* Complete or short name in an EIR block: a length, a type, the data. */
static void eir_name(struct bgpd_device *d, const uint8_t *p, size_t size)
{
	while (size > 0) {
		size_t length = p[0];

		if (length == 0 || length >= size)
			return;
		if (length > 1 && (p[1] == 8 || p[1] == 9)) {
			clean_name(d->name, p + 2, length - 1);
			if (p[1] == 9)
				return;
		}
		p += length + 1;
		size -= length + 1;
	}
}

static void apply_settings(struct bgpd_mgmt *m, uint32_t value)
{
	m->settings = value;
	m->scan_type = (uint8_t)(((value & SETTING_BREDR) != 0 ? 1U : 0U) |
				 ((value & SETTING_LE) != 0 ? 6U : 0U));
	if ((value & SETTING_POWERED) == 0) {
		m->scanning = m->own_scan = false;
		for (size_t i = 0; i < m->devices.count; i++)
			m->devices.items[i].connected = false;
	}
}

/* Hands the keys of one kind to the kernel. */
static int load_keys(struct bgpd_mgmt *m, uint16_t op, uint8_t kind)
{
	uint8_t data[3 + BGPD_MAX_KEYS * BGPD_KEY_DATA_SIZE] = {0};
	size_t header = op == BGPD_OP_LOAD_LINK_KEYS ? 3 : 2, offset = header;
	size_t count = 0;

	for (size_t i = 0; i < m->store.count; i++) {
		const struct bgpd_key *key = &m->store.keys[i];

		if (key->kind != kind)
			continue;
		memcpy(data + offset, key->data, key->length);
		offset += key->length;
		count++;
	}
	bgpd_put16(data + (header - 2), (uint16_t)count);
	return command(m, op, data, offset);
}

/* Get Connections is only valid while the adapter is powered on. */
static void finish_initialization(struct bgpd_mgmt *m)
{
	if ((m->settings & SETTING_POWERED) != 0) {
		command(m, BGPD_OP_CONNECTIONS, NULL, 0);
		return;
	}
	m->ready = true;
	say(m, "Adapter powered off: turn it on to scan");
}

/* Enables the transports lazily, then starts discovery. */
static void prepare_scan(struct bgpd_mgmt *m)
{
	uint8_t on = 1;

	if ((m->supported & SETTING_LE) != 0 &&
	    (m->settings & SETTING_LE) == 0) {
		command(m, BGPD_OP_LE, &on, 1);
		return;
	}
	if ((m->supported & SETTING_BREDR) != 0 &&
	    (m->settings & SETTING_BREDR) == 0) {
		command(m, BGPD_OP_BREDR, &on, 1);
		return;
	}
	m->prepare_action = 0;
	if (m->scan_type == 0) {
		say(m, "No Bluetooth transport available");
		return;
	}
	command(m, BGPD_OP_START_DISCOVERY, &m->scan_type, 1);
}

static void start_pair(struct bgpd_mgmt *m)
{
	uint8_t data[8];

	memcpy(data, m->pair_addr, BGPD_ADDR_SIZE);
	data[7] = IO_CAPABILITY_KEYBOARD_DISPLAY;
	if (command(m, BGPD_OP_PAIR, data, sizeof(data)) < 0)
		m->pairing = false;
	else
		m->deadline = bgpd_now() + PAIR_TIMEOUT_MS;
}

/* Enables what pairing needs (transport, bondable, SSP), one step a time. */
static void prepare_pair(struct bgpd_mgmt *m)
{
	uint32_t bit = m->pair_addr[6] != 0 ? SETTING_LE : SETTING_BREDR;
	uint8_t on = 1;

	if ((m->supported & bit) == 0) {
		m->prepare_action = 0;
		m->pairing = false;
		say(m, "Unsupported transport");
		return;
	}
	if (m->own_scan) {
		/* Pairing while discovering makes the connection fail. */
		command(m, BGPD_OP_STOP_DISCOVERY, &m->scan_type, 1);
		return;
	}
	if ((m->settings & bit) == 0) {
		command(m, m->pair_addr[6] != 0 ? BGPD_OP_LE : BGPD_OP_BREDR,
			&on, 1);
		return;
	}
	if ((m->settings & SETTING_BONDABLE) == 0) {
		command(m, BGPD_OP_BONDABLE, &on, 1);
		return;
	}
	if (m->pair_addr[6] == BGPD_ADDR_BREDR &&
	    (m->supported & SETTING_SSP) != 0 &&
	    (m->settings & SETTING_SSP) == 0) {
		command(m, BGPD_OP_SSP, &on, 1);
		return;
	}
	m->prepare_action = 0;
	start_pair(m);
}

static void prepare(struct bgpd_mgmt *m)
{
	if (m->prepare_action == BGPD_OP_START_DISCOVERY)
		prepare_scan(m);
	else if (m->prepare_action == BGPD_OP_PAIR)
		prepare_pair(m);
}

static void clear_prompt(struct bgpd_mgmt *m)
{
	m->prompt_kind = BGPD_PROMPT_NONE;
	m->prompt[0] = m->input[0] = '\0';
}

void bgpd_mgmt_scan(struct bgpd_mgmt *m)
{
	if (!m->ready || m->command != 0)
		return;
	if ((m->settings & SETTING_POWERED) == 0) {
		say(m, "Turn the adapter on first");
		return;
	}
	if (m->own_scan) {
		command(m, BGPD_OP_STOP_DISCOVERY, &m->scan_type, 1);
		return;
	}
	m->prepare_action = BGPD_OP_START_DISCOVERY;
	prepare(m);
}

void bgpd_mgmt_pair(struct bgpd_mgmt *m, const struct bgpd_device *device)
{
	if (!m->ready || m->command != 0 || m->pairing)
		return;
	if ((m->settings & SETTING_POWERED) == 0) {
		say(m, "Turn the adapter on first");
		return;
	}
	if (device->paired) {
		say(m, "Already paired");
		return;
	}
	memcpy(m->pair_addr, device->addr, BGPD_ADDR_SIZE);
	memcpy(m->pair_original, device->addr, BGPD_ADDR_SIZE);
	m->pairing = true;
	m->prepare_action = BGPD_OP_PAIR;
	prepare(m);
}

void bgpd_mgmt_cancel_pair(struct bgpd_mgmt *m)
{
	if (m->pairing)
		send_command(m, BGPD_OP_CANCEL_PAIR, m->index, m->pair_original,
			     BGPD_ADDR_SIZE);
}

void bgpd_mgmt_unpair(struct bgpd_mgmt *m, const struct bgpd_device *device)
{
	uint8_t data[8];

	if (!m->ready || m->command != 0)
		return;
	memcpy(data, device->addr, BGPD_ADDR_SIZE);
	data[7] = 1; /* disconnect the device too */
	memcpy(m->unpair_addr, device->addr, BGPD_ADDR_SIZE);
	command(m, BGPD_OP_UNPAIR, data, sizeof(data));
}

void bgpd_mgmt_disconnect(struct bgpd_mgmt *m, const struct bgpd_device *device)
{
	if (!m->ready || m->command != 0)
		return;
	command(m, BGPD_OP_DISCONNECT, device->addr, BGPD_ADDR_SIZE);
}

void bgpd_mgmt_toggle_power(struct bgpd_mgmt *m)
{
	uint8_t on = (m->settings & SETTING_POWERED) == 0;

	if (!m->ready || m->command != 0)
		return;
	command(m, BGPD_OP_POWER, &on, 1);
}

void bgpd_mgmt_next_adapter(struct bgpd_mgmt *m)
{
	if (m->command != 0 || m->own_scan) {
		say(m, "Stop scanning before switching adapters");
		return;
	}
	if (!m->ready) {
		m->requested_index = BGPD_NO_INDEX;
		command(m, BGPD_OP_INDEX_LIST, NULL, 0);
		return;
	}
	for (size_t i = 0; i < m->controller_count; i++) {
		if (m->controllers[i] == m->index) {
			m->index =
			    m->controllers[(i + 1) % m->controller_count];
			break;
		}
	}
	m->devices.count = 0;
	m->ready = false;
	command(m, BGPD_OP_INFO, NULL, 0);
}

static bool known_controller(const struct bgpd_mgmt *m, uint16_t index)
{
	for (size_t i = 0; i < m->controller_count; i++)
		if (m->controllers[i] == index)
			return true;
	return false;
}

static void on_index_list(struct bgpd_mgmt *m, const uint8_t *p, size_t size,
			  bool *ok)
{
	size_t count;

	if (size < 2 || size != 2 + (size_t)bgpd_get16(p) * 2) {
		*ok = false;
		return;
	}
	count = bgpd_get16(p);
	if (count > BGPD_MAX_CONTROLLERS)
		count = BGPD_MAX_CONTROLLERS;
	m->controller_count = count;
	for (size_t i = 0; i < count; i++)
		m->controllers[i] = bgpd_get16(p + 2 + 2 * i);
	if (count == 0) {
		say(m, "No Bluetooth adapter");
		return;
	}
	m->index = m->requested_index == BGPD_NO_INDEX ? m->controllers[0]
						       : m->requested_index;
	if (!known_controller(m, m->index)) {
		say(m, "Adapter hci%u is absent", m->index);
		return;
	}
	m->devices.count = 0;
	m->store.count = 0;
	command(m, BGPD_OP_INFO, NULL, 0);
}

static void on_info(struct bgpd_mgmt *m, const uint8_t *p, size_t size,
		    bool *ok)
{
	char file[BGPD_STORE_FILE_SIZE];

	if (size < INFO_SIZE) {
		*ok = false;
		return;
	}
	m->supported = bgpd_get32(p + 9);
	apply_settings(m, bgpd_get32(p + 13));
	bgpd_address_format(p, m->controller_address);
	clean_name(m->controller_name, p + 20,
		   strnlen((const char *)p + 20, 248));
	snprintf(file, sizeof(file), "%02X%02X%02X%02X%02X%02X.keys", p[5],
		 p[4], p[3], p[2], p[1], p[0]);
	m->storage_error = true;
	if ((m->store.dirfd < 0 &&
	     bgpd_store_open(&m->store, m->state_dir) < 0) ||
	    bgpd_store_load(&m->store, file) < 0) {
		say(m, "Key storage error: %s", strerror(errno));
		return;
	}
	m->storage_error = false;
	bgpd_store_mark(&m->store, &m->devices);
	if ((m->supported & SETTING_BREDR) != 0)
		load_keys(m, BGPD_OP_LOAD_LINK_KEYS, BGPD_KEY_LINK);
	else if ((m->supported & SETTING_LE) != 0)
		load_keys(m, BGPD_OP_LOAD_LTKS, BGPD_KEY_LTK);
	else
		finish_initialization(m);
}

static void on_connections(struct bgpd_mgmt *m, const uint8_t *p, size_t size,
			   bool *ok)
{
	size_t count;

	if (size < 2 || size != 2 + (size_t)bgpd_get16(p) * BGPD_ADDR_SIZE) {
		*ok = false;
		return;
	}
	count = bgpd_get16(p);
	for (size_t i = 0; i < count; i++) {
		struct bgpd_device *d =
		    bgpd_devices_get(&m->devices, p + 2 + i * BGPD_ADDR_SIZE);

		if (d != NULL)
			d->connected = true;
	}
	m->ready = true;
	say(m, "Kernel MGMT ready: s scan");
}

static void forget(struct bgpd_mgmt *m, const uint8_t *addr);

static void complete(struct bgpd_mgmt *m, uint16_t op, uint8_t code,
		     const uint8_t *p, size_t size)
{
	bool ok = true;

	if (op != m->command) {
		if (code != 0)
			say(m, "MGMT 0x%04x: %s", op, error_name(code));
		return;
	}
	m->command = 0;
	if (code != 0) {
		if (op == BGPD_OP_CONNECTIONS &&
		    code == STATUS_OPERATION_NOT_ALLOWED_OFF) {
			apply_settings(m, m->settings & ~SETTING_POWERED);
			finish_initialization(m);
			return;
		}
		m->prepare_action = 0;
		say(m, "MGMT 0x%04x: %s", op, error_name(code));
		if (m->pairing) {
			m->pairing = false;
			clear_prompt(m);
		}
		return;
	}
	switch (op) {
	case BGPD_OP_INDEX_LIST:
		on_index_list(m, p, size, &ok);
		break;
	case BGPD_OP_INFO:
		on_info(m, p, size, &ok);
		break;
	case BGPD_OP_LOAD_LINK_KEYS:
		if ((m->supported & SETTING_LE) != 0)
			load_keys(m, BGPD_OP_LOAD_LTKS, BGPD_KEY_LTK);
		else
			finish_initialization(m);
		break;
	case BGPD_OP_LOAD_LTKS:
		if ((m->supported & SETTING_LE) != 0)
			load_keys(m, BGPD_OP_LOAD_IRKS, BGPD_KEY_IRK);
		else
			finish_initialization(m);
		break;
	case BGPD_OP_LOAD_IRKS:
		finish_initialization(m);
		break;
	case BGPD_OP_CONNECTIONS:
		on_connections(m, p, size, &ok);
		break;
	case BGPD_OP_POWER:
	case BGPD_OP_BONDABLE:
	case BGPD_OP_SSP:
	case BGPD_OP_LE:
	case BGPD_OP_BREDR:
		if (size < 4) {
			ok = false;
			break;
		}
		apply_settings(m, bgpd_get32(p));
		if (m->prepare_action != 0)
			prepare(m);
		else
			say(m, "MGMT 0x%04x: OK", op);
		break;
	case BGPD_OP_PAIR: {
		struct bgpd_device *d;

		if (size < BGPD_ADDR_SIZE) {
			ok = false;
			break;
		}
		d = bgpd_devices_get(&m->devices, p);
		if (d != NULL)
			d->paired = true;
		m->pairing = false;
		clear_prompt(m);
		say(m, "Pairing successful%s",
		    m->storage_error ? "; ERROR saving keys"
		    : d != NULL && d->saved
			? "; keys saved"
			: "; temporary bond without a persistent key");
		break;
	}
	case BGPD_OP_UNPAIR:
		forget(m, m->unpair_addr);
		if (!m->storage_error)
			say(m, "Device forgotten");
		break;
	case BGPD_OP_DISCONNECT: {
		struct bgpd_device *d = size >= BGPD_ADDR_SIZE
					    ? bgpd_devices_get(&m->devices, p)
					    : NULL;

		if (d != NULL)
			d->connected = false;
		say(m, "Disconnected");
		break;
	}
	case BGPD_OP_START_DISCOVERY:
		m->own_scan = m->scanning = true;
		say(m, "Scanning");
		break;
	case BGPD_OP_STOP_DISCOVERY:
		m->own_scan = m->scanning = false;
		if (m->prepare_action != 0)
			prepare(m);
		else
			say(m, "Scan stopped");
		break;
	default:
		say(m, "MGMT 0x%04x: OK", op);
		break;
	}
	if (!ok) {
		m->prepare_action = 0;
		m->pairing = false;
		clear_prompt(m);
		say(m, "Truncated MGMT 0x%04x response", op);
	}
}

static void on_device_found(struct bgpd_mgmt *m, uint16_t index,
			    const uint8_t *p, size_t size)
{
	struct bgpd_device *d;

	if (size < 14 || size != 14 + (size_t)bgpd_get16(p + 12))
		return;
	d = bgpd_devices_get(&m->devices, p);
	if (d == NULL)
		return;
	d->rssi = p[7] >= 128 ? (int)p[7] - 256 : (int)p[7];
	d->has_rssi = p[7] != 127;
	eir_name(d, p + 14, size - 14);
	if ((bgpd_get32(p + 8) & 1U) != 0) {
		uint8_t reply[8];

		/* Ask the kernel to resolve the name unless we have one. */
		memcpy(reply, p, BGPD_ADDR_SIZE);
		reply[7] = strlen(d->name) != 17;
		send_command(m, BGPD_OP_CONFIRM_NAME, index, reply,
			     sizeof(reply));
	}
}

static void on_index_removed(struct bgpd_mgmt *m, uint16_t index)
{
	for (size_t i = 0; i < m->controller_count; i++) {
		if (m->controllers[i] != index)
			continue;
		memmove(m->controllers + i, m->controllers + i + 1,
			(m->controller_count - i - 1) *
			    sizeof(*m->controllers));
		m->controller_count--;
		break;
	}
	if (index != m->index)
		return;
	m->ready = m->own_scan = m->scanning = m->pairing = false;
	m->command = 0;
	clear_prompt(m);
	say(m, "Adapter removed; a to refresh");
}

static void on_key(struct bgpd_mgmt *m, uint8_t kind, const uint8_t *data,
		   size_t length)
{
	struct bgpd_device *d;

	if (bgpd_store_add(&m->store, kind, data, length) < 0) {
		m->storage_error = true;
		say(m, "Key storage error: %s", strerror(errno));
		return;
	}
	m->storage_error = false;
	d = bgpd_devices_get(&m->devices, data);
	if (d != NULL && kind != BGPD_KEY_IRK)
		d->paired = d->saved = true;
}

static void forget(struct bgpd_mgmt *m, const uint8_t *addr)
{
	struct bgpd_device *d;

	if (bgpd_store_forget(&m->store, addr) < 0) {
		m->storage_error = true;
		say(m, "Key storage error: %s", strerror(errno));
	}
	d = bgpd_devices_get(&m->devices, addr);
	if (d != NULL)
		d->paired = d->saved = d->connected = false;
}

/*
 * A device that pairs with a random address announces its identity
 * address: the entry of the random one moves to the identity.
 */
static void on_new_irk(struct bgpd_mgmt *m, const uint8_t *p)
{
	struct bgpd_device *old = NULL, *identity;

	for (size_t i = 0; i < m->devices.count; i++) {
		struct bgpd_device *d = &m->devices.items[i];

		if (d->addr[6] == BGPD_ADDR_LE_RANDOM &&
		    memcmp(d->addr, p + 1, 6) == 0)
			old = d;
	}
	identity = bgpd_devices_get(&m->devices, p + 7);
	if (old != NULL && identity != NULL && old != identity) {
		snprintf(identity->name, sizeof(identity->name), "%s",
			 old->name);
		old->paired = old->saved = false;
	}
	if (m->pairing && m->pair_addr[6] == BGPD_ADDR_LE_RANDOM &&
	    memcmp(m->pair_addr, p + 1, 6) == 0)
		memcpy(m->pair_addr, p + 7, BGPD_ADDR_SIZE);
	if (p[0] != 0)
		on_key(m, BGPD_KEY_IRK, p + 7, 23);
}

/* Turns a PIN, confirmation or passkey request into a prompt. */
static void on_auth_request(struct bgpd_mgmt *m, uint16_t event, uint16_t index,
			    const uint8_t *p, size_t size)
{
	const size_t sizes[] = {8, 12, 7}; /* PIN, confirmation, passkey */
	char address[BGPD_ADDR_TEXT];

	if (size != sizes[event - EV_PIN_REQUEST])
		return;
	if (!m->pairing || memcmp(m->pair_addr, p, BGPD_ADDR_SIZE) != 0) {
		/* Unsolicited requests are refused. */
		uint16_t no = event == EV_PIN_REQUEST ? BGPD_OP_PIN_NEGATIVE
			      : event == EV_USER_PASSKEY
				  ? BGPD_OP_PASSKEY_NEGATIVE
				  : BGPD_OP_CONFIRM_NEGATIVE;

		send_command(m, no, index, p, BGPD_ADDR_SIZE);
		return;
	}
	m->prompt_kind = event;
	memcpy(m->prompt_addr, p, BGPD_ADDR_SIZE);
	m->input[0] = '\0';
	bgpd_address_format(p, address);
	m->secure_pin = event == EV_PIN_REQUEST && p[7] != 0;
	if (event == EV_USER_CONFIRM && p[7] != 0)
		snprintf(m->prompt, sizeof(m->prompt), "Pair %s? [y/n]",
			 address);
	else if (event == EV_USER_CONFIRM)
		snprintf(m->prompt, sizeof(m->prompt),
			 "%s: confirm %06u? [y/n]", address, bgpd_get32(p + 8));
	else
		snprintf(m->prompt, sizeof(m->prompt),
			 "%s: %s; Enter accepts, Esc rejects", address,
			 event == EV_USER_PASSKEY ? "passkey: 1-6 digits"
			 : m->secure_pin	  ? "PIN: 16 characters"
						  : "PIN: 1-16 characters");
}

void bgpd_mgmt_reply(struct bgpd_mgmt *m, bool accept)
{
	uint8_t data[24] = {0};
	size_t size = BGPD_ADDR_SIZE;
	uint16_t op;

	if (m->prompt_kind == BGPD_PROMPT_NONE)
		return;
	memcpy(data, m->prompt_addr, BGPD_ADDR_SIZE);
	if (m->prompt_kind == BGPD_PROMPT_PIN) {
		size_t length = strlen(m->input);

		op = accept ? BGPD_OP_PIN_REPLY : BGPD_OP_PIN_NEGATIVE;
		if (accept) {
			if (length == 0 ||
			    (m->secure_pin && length != SECURE_PIN_LENGTH)) {
				say(m, "Secure PIN: 16 characters required");
				return;
			}
			data[7] = (uint8_t)length;
			memcpy(data + 8, m->input, length);
			size = sizeof(data);
		}
	} else if (m->prompt_kind == BGPD_PROMPT_PASSKEY) {
		op = accept ? BGPD_OP_PASSKEY : BGPD_OP_PASSKEY_NEGATIVE;
		if (accept) {
			uint32_t value = 0;

			if (m->input[0] == '\0')
				return;
			for (const char *c = m->input; *c != '\0'; c++)
				value = value * 10U + (uint32_t)(*c - '0');
			bgpd_put32(data + 7, value);
			size = 11;
		}
	} else {
		op = accept ? BGPD_OP_CONFIRM : BGPD_OP_CONFIRM_NEGATIVE;
	}
	if (send_command(m, op, m->index, data, size) == 0)
		clear_prompt(m);
}

static void on_event(struct bgpd_mgmt *m, uint16_t event, uint16_t index,
		     const uint8_t *p, size_t size)
{
	struct bgpd_device *d;

	if (event == EV_INDEX_ADDED && size == 0) {
		if (!known_controller(m, index) &&
		    m->controller_count < BGPD_MAX_CONTROLLERS)
			m->controllers[m->controller_count++] = index;
		return;
	}
	if (event == EV_INDEX_REMOVED && size == 0)
		on_index_removed(m, index);
	if (index != m->index)
		return;
	switch (event) {
	case EV_NEW_SETTINGS:
		if (size == 4)
			apply_settings(m, bgpd_get32(p));
		break;
	case EV_DEVICE_FOUND:
		on_device_found(m, index, p, size);
		break;
	case EV_DISCOVERING:
		if (size == 2)
			m->scanning = p[1] != 0;
		break;
	case EV_CONNECTED:
		if (size < 13 || size != 13 + (size_t)bgpd_get16(p + 11))
			break;
		d = bgpd_devices_get(&m->devices, p);
		if (d != NULL) {
			d->connected = true;
			eir_name(d, p + 13, size - 13);
		}
		break;
	case EV_DISCONNECTED:
	case EV_UNPAIRED:
		if (size < BGPD_ADDR_SIZE)
			break;
		d = bgpd_devices_get(&m->devices, p);
		if (d != NULL)
			d->connected = false;
		if (event == EV_UNPAIRED)
			forget(m, p);
		break;
	case EV_NEW_LINK_KEY:
		if (size == 26 && p[0] != 0)
			on_key(m, BGPD_KEY_LINK, p + 1, 25);
		break;
	case EV_NEW_LTK:
		if (size == 37 && p[0] != 0)
			on_key(m, BGPD_KEY_LTK, p + 1, 36);
		break;
	case EV_NEW_IRK:
		if (size == 30)
			on_new_irk(m, p);
		break;
	case EV_PIN_REQUEST:
	case EV_USER_CONFIRM:
	case EV_USER_PASSKEY:
		on_auth_request(m, event, index, p, size);
		break;
	case EV_PASSKEY_NOTIFY:
		if (size == 12 && m->pairing &&
		    memcmp(m->pair_addr, p, BGPD_ADDR_SIZE) == 0)
			say(m, "Enter %06u on the device (%u digits entered)",
			    bgpd_get32(p + 7), p[11]);
		break;
	case EV_AUTH_FAILED:
		if (size == 8 && m->pairing &&
		    memcmp(m->pair_addr, p, BGPD_ADDR_SIZE) == 0) {
			clear_prompt(m);
			say(m, "Authentication: %s", error_name(p[7]));
		}
		break;
	default:
		break;
	}
}

void bgpd_mgmt_packet(struct bgpd_mgmt *m, const uint8_t *packet, size_t size)
{
	uint16_t event, index;
	const uint8_t *p = packet + HEADER_SIZE;

	if (size < HEADER_SIZE || size != HEADER_SIZE + bgpd_get16(packet + 4))
		return;
	event = bgpd_get16(packet);
	index = bgpd_get16(packet + 2);
	size -= HEADER_SIZE;
	trace(m, "<", event, index, p, size,
	      event == EV_NEW_LINK_KEY || event == EV_NEW_LTK ||
		  event == EV_NEW_IRK);
	if (event == EV_COMMAND_COMPLETE || event == EV_COMMAND_STATUS) {
		uint16_t op;

		if (size < 3)
			return;
		op = bgpd_get16(p);
		if (index !=
		    (op == BGPD_OP_INDEX_LIST ? BGPD_NO_INDEX : m->index))
			return;
		if (event == EV_COMMAND_STATUS && p[2] == 0)
			return; /* an early success is not the completion */
		complete(m, op, p[2], p + 3, size - 3);
		return;
	}
	on_event(m, event, index, p, size);
}

void bgpd_mgmt_poll(struct bgpd_mgmt *m)
{
	uint8_t data[MAX_PACKET];

	for (int i = 0; i < 64 && m->fd >= 0; i++) {
		ssize_t n = read(m->fd, data, sizeof(data));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			say(m, "MGMT socket closed: %s",
			    n != 0 ? strerror(errno) : "end of file");
			m->ready = false;
			break;
		}
		bgpd_mgmt_packet(m, data, (size_t)n);
	}
	if (m->command != 0 && bgpd_now() > m->deadline) {
		bgpd_mgmt_cancel_pair(m);
		m->pairing = false;
		clear_prompt(m);
		m->command = 0;
		m->prepare_action = 0;
		say(m, "MGMT timeout");
	}
}

void bgpd_mgmt_close(struct bgpd_mgmt *m)
{
	if (m->fd >= 0) {
		if (m->prompt_kind != BGPD_PROMPT_NONE)
			bgpd_mgmt_reply(m, false);
		bgpd_mgmt_cancel_pair(m);
		if (m->own_scan)
			send_command(m, BGPD_OP_STOP_DISCOVERY, m->index,
				     &m->scan_type, 1);
		close(m->fd);
	}
	m->fd = -1;
	bgpd_store_close(&m->store);
	if (m->trace != NULL)
		fclose(m->trace);
	m->trace = NULL;
}
