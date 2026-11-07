/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "mgmt.h"

#include <errno.h>
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

static void prepare(struct bgpd_mgmt *m)
{
	if (m->prepare_action == BGPD_OP_START_DISCOVERY)
		prepare_scan(m);
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
	case BGPD_OP_START_DISCOVERY:
		m->own_scan = m->scanning = true;
		say(m, "Scanning");
		break;
	case BGPD_OP_STOP_DISCOVERY:
		m->own_scan = m->scanning = false;
		say(m, "Scan stopped");
		break;
	default:
		say(m, "MGMT 0x%04x: OK", op);
		break;
	}
	if (!ok) {
		m->prepare_action = 0;
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
	m->ready = m->own_scan = m->scanning = false;
	m->command = 0;
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
		if (size == 30 && p[0] != 0)
			on_key(m, BGPD_KEY_IRK, p + 7, 23);
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
		m->command = 0;
		m->prepare_action = 0;
		say(m, "MGMT timeout");
	}
}

void bgpd_mgmt_close(struct bgpd_mgmt *m)
{
	if (m->fd >= 0) {
		if (m->own_scan)
			send_command(m, BGPD_OP_STOP_DISCOVERY, m->index,
				     &m->scan_type, 1);
		close(m->fd);
	}
	m->fd = -1;
	bgpd_store_close(&m->store);
}
