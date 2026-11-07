/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "hog.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uhid.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Kernel values, so that no BlueZ header is needed. */
#define AF_BLUETOOTH_FAMILY 31
#define BTPROTO_L2CAP 0
#define SOL_BLUETOOTH_LEVEL 274
#define BT_SECURITY_OPTION 4
#define BT_SECURITY_MEDIUM 2
#define ATT_CID 4
#define BUS_BLUETOOTH_ID 0x05

struct sockaddr_l2cap {
	sa_family_t family;
	uint16_t psm;
	uint8_t addr[6];
	uint16_t cid;
	uint8_t addr_type;
};

struct bt_security_option {
	uint8_t level, key_size;
};

/* ATT opcodes. */
#define ATT_ERROR 0x01
#define ATT_FIND_INFO_REQ 0x04
#define ATT_FIND_INFO_RSP 0x05
#define ATT_READ_BY_TYPE_REQ 0x08
#define ATT_READ_BY_TYPE_RSP 0x09
#define ATT_READ_REQ 0x0a
#define ATT_READ_RSP 0x0b
#define ATT_READ_BLOB_REQ 0x0c
#define ATT_READ_BLOB_RSP 0x0d
#define ATT_READ_BY_GROUP_REQ 0x10
#define ATT_READ_BY_GROUP_RSP 0x11
#define ATT_WRITE_REQ 0x12
#define ATT_WRITE_RSP 0x13
#define ATT_NOTIFY 0x1b
#define ATT_INDICATE 0x1d
#define ATT_CONFIRM 0x1e
#define ATT_NOT_FOUND 0x0a

/* UUIDs. */
#define UUID_PRIMARY 0x2800
#define UUID_CHARACTERISTIC 0x2803
#define UUID_CCCD 0x2902
#define UUID_REPORT_REFERENCE 0x2908
#define UUID_HID_SERVICE 0x1812
#define UUID_REPORT_MAP 0x2a4b
#define UUID_REPORT 0x2a4d

#define PROP_NOTIFY 0x10U
#define REPORT_INPUT 1
#define DEFAULT_MTU 23
#define STEP_TIMEOUT_MS 10000
#define CONNECT_TIMEOUT_MS 30000
#define PACKET_SIZE 512

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((unsigned int)p[1] << 8U));
}

static void put_le16(uint8_t *p, uint16_t n)
{
	p[0] = (uint8_t)(n & 0xffU);
	p[1] = (uint8_t)(n >> 8U);
}

static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}

static void say(struct bgpd_hog *hog, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void say(struct bgpd_hog *hog, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(hog->status, sizeof(hog->status), format, args);
	va_end(args);
	hog->changed = true;
	if (hog->trace != NULL) {
		fprintf(hog->trace, "%8llu ms hog %s\n",
			(unsigned long long)(now_ms() - hog->trace_start),
			hog->status);
		fflush(hog->trace);
	}
}

void bgpd_hog_init(struct bgpd_hog *hog)
{
	memset(hog, 0, sizeof(*hog));
	hog->att = hog->uhid = -1;
	hog->uhid_path = "/dev/uhid";
}

bool bgpd_hog_active(const struct bgpd_hog *hog)
{
	return hog->state != BGPD_HOG_IDLE;
}

void bgpd_hog_close(struct bgpd_hog *hog)
{
	if (hog->uhid >= 0) {
		struct uhid_event ev;

		memset(&ev, 0, sizeof(ev));
		ev.type = UHID_DESTROY;
		ssize_t written = write(hog->uhid, &ev, sizeof(ev));

		(void)written; /* the device goes away with the descriptor */
		close(hog->uhid);
	}
	if (hog->att >= 0)
		close(hog->att);
	hog->att = hog->uhid = -1;
	hog->state = BGPD_HOG_IDLE;
}

static void fail(struct bgpd_hog *hog, const char *what)
{
	say(hog, "%s", what);
	bgpd_hog_close(hog);
}

static int send_att(struct bgpd_hog *hog, const uint8_t *pdu, size_t size)
{
	ssize_t n;

	do {
		n = write(hog->att, pdu, size);
	} while (n < 0 && errno == EINTR);
	if (n != (ssize_t)size) {
		fail(hog, "Lost the connection to the device");
		return -1;
	}
	hog->deadline = now_ms() + STEP_TIMEOUT_MS;
	if (hog->trace != NULL) {
		fprintf(hog->trace, "%8llu ms hog > att 0x%02x (%zu bytes)\n",
			(unsigned long long)(now_ms() - hog->trace_start),
			pdu[0], size);
		fflush(hog->trace);
	}
	return 0;
}

/* Read By Group Type / Read By Type / Find Information on a range. */
static int request_range(struct bgpd_hog *hog, uint8_t opcode, uint16_t start,
			 uint16_t end, uint16_t type)
{
	uint8_t pdu[7];

	pdu[0] = opcode;
	put_le16(pdu + 1, start);
	put_le16(pdu + 3, end);
	if (opcode == ATT_FIND_INFO_REQ)
		return send_att(hog, pdu, 5);
	put_le16(pdu + 5, type);
	return send_att(hog, pdu, sizeof(pdu));
}

static int request_handle(struct bgpd_hog *hog, uint8_t opcode, uint16_t handle,
			  uint16_t offset)
{
	uint8_t pdu[5];

	pdu[0] = opcode;
	put_le16(pdu + 1, handle);
	put_le16(pdu + 3, offset);
	return send_att(hog, pdu, opcode == ATT_READ_BLOB_REQ ? 5 : 3);
}

static struct bgpd_hog_char *find_char(struct bgpd_hog *hog, uint16_t uuid)
{
	for (size_t i = 0; i < hog->count; i++)
		if (hog->chars[i].uuid == uuid)
			return &hog->chars[i];
	return NULL;
}

/* Input reports we can subscribe to, in the order of the service. */
static struct bgpd_hog_char *next_input(struct bgpd_hog *hog)
{
	for (; hog->next < hog->count; hog->next++) {
		struct bgpd_hog_char *c = &hog->chars[hog->next];

		if (c->uuid == UUID_REPORT &&
		    ((unsigned int)c->props & PROP_NOTIFY) != 0)
			return c;
	}
	return NULL;
}

static int create_device(struct bgpd_hog *hog)
{
	struct uhid_event ev;
	ssize_t n;

	hog->uhid =
	    open(hog->uhid_path, (int)(O_RDWR | O_CLOEXEC | O_NONBLOCK));
	if (hog->uhid < 0) {
		say(hog, "Cannot open %s: %s", hog->uhid_path, strerror(errno));
		return -1;
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%.127s",
		 hog->name);
	ev.u.create2.rd_size = (uint16_t)hog->map_size;
	ev.u.create2.bus = BUS_BLUETOOTH_ID;
	memcpy(ev.u.create2.rd_data, hog->map, hog->map_size);
	do {
		n = write(hog->uhid, &ev, sizeof(ev));
	} while (n < 0 && errno == EINTR);
	if (n != (ssize_t)sizeof(ev)) {
		say(hog, "Cannot create the input device: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int subscribe_next(struct bgpd_hog *hog)
{
	struct bgpd_hog_char *c = next_input(hog);
	uint8_t pdu[5];

	if (c == NULL) {
		if (create_device(hog) < 0) {
			bgpd_hog_close(hog);
			return -1;
		}
		hog->state = BGPD_HOG_RUNNING;
		hog->deadline = 0;
		say(hog, "%s is connected as an input device", hog->name);
		return 0;
	}
	if (c->cccd == 0) {
		hog->next++;
		return subscribe_next(hog);
	}
	pdu[0] = ATT_WRITE_REQ;
	put_le16(pdu + 1, c->cccd);
	put_le16(pdu + 3, 0x0001); /* notifications */
	return send_att(hog, pdu, sizeof(pdu));
}

static int read_references(struct bgpd_hog *hog)
{
	for (; hog->next < hog->count; hog->next++) {
		struct bgpd_hog_char *c = &hog->chars[hog->next];

		if (c->uuid == UUID_REPORT && c->reference != 0)
			return request_handle(hog, ATT_READ_REQ, c->reference,
					      0);
	}
	hog->state = BGPD_HOG_SUBSCRIBE;
	hog->next = 0;
	return subscribe_next(hog);
}

static int start_map(struct bgpd_hog *hog)
{
	struct bgpd_hog_char *map = find_char(hog, UUID_REPORT_MAP);

	if (map == NULL) {
		fail(hog, "The HID service has no report map");
		return -1;
	}
	hog->state = BGPD_HOG_MAP;
	hog->map_size = 0;
	return request_handle(hog, ATT_READ_REQ, map->value, 0);
}

/* A characteristic declaration owns the descriptors that follow it. */
static void on_descriptors(struct bgpd_hog *hog, const uint8_t *p, size_t size)
{
	uint16_t last = 0;

	if (size < 2 || p[0] != 1) {
		fail(hog, "Unsupported descriptor list");
		return;
	}
	for (size_t i = 1; i + 4 <= size; i += 4) {
		uint16_t handle = le16(p + i), uuid = le16(p + i + 2);
		struct bgpd_hog_char *owner = NULL;

		last = handle;
		for (size_t k = 0; k < hog->count; k++)
			if (hog->chars[k].value < handle)
				owner = &hog->chars[k];
		if (owner == NULL)
			continue;
		if (uuid == UUID_CCCD)
			owner->cccd = handle;
		else if (uuid == UUID_REPORT_REFERENCE)
			owner->reference = handle;
	}
	if (last == 0 || last >= hog->service_end) {
		start_map(hog);
		return;
	}
	hog->cursor = (uint16_t)(last + 1);
	request_range(hog, ATT_FIND_INFO_REQ, hog->cursor, hog->service_end, 0);
}

static void on_characteristics(struct bgpd_hog *hog, const uint8_t *p,
			       size_t size)
{
	size_t length;
	uint16_t last = 0;

	if (size < 1 || p[0] < 7) {
		fail(hog, "Unsupported characteristic list");
		return;
	}
	length = p[0];
	for (size_t i = 1; i + length <= size; i += length) {
		struct bgpd_hog_char *c;

		last = le16(p + i);
		if (hog->count == BGPD_HOG_MAX_CHARS || length != 7)
			continue;
		c = &hog->chars[hog->count++];
		memset(c, 0, sizeof(*c));
		c->props = p[i + 2];
		c->value = le16(p + i + 3);
		c->uuid = le16(p + i + 5);
	}
	if (last != 0 && last < hog->service_end) {
		hog->cursor = (uint16_t)(last + 1);
		request_range(hog, ATT_READ_BY_TYPE_REQ, hog->cursor,
			      hog->service_end, UUID_CHARACTERISTIC);
		return;
	}
	hog->state = BGPD_HOG_DESCRIPTORS;
	hog->cursor = hog->service_start;
	request_range(hog, ATT_FIND_INFO_REQ, hog->cursor, hog->service_end, 0);
}

static void on_services(struct bgpd_hog *hog, const uint8_t *p, size_t size)
{
	size_t length;
	uint16_t last_end = 0;

	if (size < 1 || p[0] < 6) {
		fail(hog, "Unsupported service list");
		return;
	}
	length = p[0];
	for (size_t i = 1; i + length <= size; i += length) {
		last_end = le16(p + i + 2);
		if (length == 6 && le16(p + i + 4) == UUID_HID_SERVICE) {
			hog->service_start = le16(p + i);
			hog->service_end = last_end;
		}
	}
	if (hog->service_start != 0) {
		hog->state = BGPD_HOG_CHARACTERISTICS;
		hog->cursor = hog->service_start;
		request_range(hog, ATT_READ_BY_TYPE_REQ, hog->cursor,
			      hog->service_end, UUID_CHARACTERISTIC);
		return;
	}
	if (last_end == 0 || last_end == 0xffff) {
		fail(hog, "The device has no HID service");
		return;
	}
	request_range(hog, ATT_READ_BY_GROUP_REQ, (uint16_t)(last_end + 1),
		      0xffff, UUID_PRIMARY);
}

static void on_map(struct bgpd_hog *hog, uint8_t opcode, const uint8_t *p,
		   size_t size)
{
	struct bgpd_hog_char *map = find_char(hog, UUID_REPORT_MAP);

	if (map == NULL || hog->map_size + size > BGPD_HOG_MAP_SIZE) {
		fail(hog, "The report map is too large");
		return;
	}
	(void)opcode;
	memcpy(hog->map + hog->map_size, p, size);
	hog->map_size += size;
	/* A full answer means that more may follow. */
	if (size == DEFAULT_MTU - 1) {
		request_handle(hog, ATT_READ_BLOB_REQ, map->value,
			       (uint16_t)hog->map_size);
		return;
	}
	hog->state = BGPD_HOG_REFERENCES;
	hog->next = 0;
	read_references(hog);
}

static void on_pdu(struct bgpd_hog *hog, const uint8_t *pdu, size_t size)
{
	if (hog->trace != NULL) {
		fprintf(hog->trace, "%8llu ms hog < att 0x%02x (%zu bytes)\n",
			(unsigned long long)(now_ms() - hog->trace_start),
			pdu[0], size);
		fflush(hog->trace);
	}
	const uint8_t *p = pdu + 1;
	size_t n = size - 1;

	if (pdu[0] == ATT_INDICATE && size >= 3) {
		uint8_t confirm = ATT_CONFIRM;

		if (write(hog->att, &confirm, 1) < 0)
			return;
	}
	if (pdu[0] == ATT_NOTIFY || pdu[0] == ATT_INDICATE) {
		/* Input report: forwarded once the device exists. */
		struct uhid_event ev;
		uint16_t handle;
		size_t offset = 0;

		if (hog->state != BGPD_HOG_RUNNING || n < 2)
			return;
		handle = le16(p);
		memset(&ev, 0, sizeof(ev));
		ev.type = UHID_INPUT2;
		for (size_t i = 0; i < hog->count; i++) {
			if (hog->chars[i].value != handle)
				continue;
			if (hog->chars[i].report_id != 0)
				ev.u.input2.data[offset++] =
				    hog->chars[i].report_id;
			if (n - 2 + offset > sizeof(ev.u.input2.data))
				return;
			memcpy(ev.u.input2.data + offset, p + 2, n - 2);
			ev.u.input2.size = (uint16_t)(n - 2 + offset);
			if (write(hog->uhid, &ev, sizeof(ev)) < 0 &&
			    errno != EAGAIN)
				fail(hog, "The input device went away");
			return;
		}
		return;
	}
	if (pdu[0] == ATT_ERROR && n >= 4) {
		uint8_t request = p[0], code = p[3];

		if (code == ATT_NOT_FOUND &&
		    (request == ATT_READ_BY_TYPE_REQ ||
		     request == ATT_FIND_INFO_REQ) &&
		    (hog->state == BGPD_HOG_CHARACTERISTICS ||
		     hog->state == BGPD_HOG_DESCRIPTORS)) {
			if (hog->state == BGPD_HOG_CHARACTERISTICS) {
				hog->state = BGPD_HOG_DESCRIPTORS;
				hog->cursor = hog->service_start;
				request_range(hog, ATT_FIND_INFO_REQ,
					      hog->cursor, hog->service_end, 0);
			} else {
				start_map(hog);
			}
			return;
		}
		if (code == ATT_NOT_FOUND && hog->state == BGPD_HOG_SERVICES) {
			fail(hog, "The device has no HID service");
			return;
		}
		fail(hog, "The device refused a request (security?)");
		return;
	}
	switch (hog->state) {
	case BGPD_HOG_SERVICES:
		if (pdu[0] == ATT_READ_BY_GROUP_RSP)
			on_services(hog, p, n);
		break;
	case BGPD_HOG_CHARACTERISTICS:
		if (pdu[0] == ATT_READ_BY_TYPE_RSP)
			on_characteristics(hog, p, n);
		break;
	case BGPD_HOG_DESCRIPTORS:
		if (pdu[0] == ATT_FIND_INFO_RSP)
			on_descriptors(hog, p, n);
		break;
	case BGPD_HOG_MAP:
		if (pdu[0] == ATT_READ_RSP || pdu[0] == ATT_READ_BLOB_RSP)
			on_map(hog, pdu[0], p, n);
		break;
	case BGPD_HOG_REFERENCES:
		if (pdu[0] == ATT_READ_RSP && n >= 2) {
			hog->chars[hog->next].report_id = p[0];
			hog->chars[hog->next].report_type = p[1];
			hog->next++;
			read_references(hog);
		}
		break;
	case BGPD_HOG_SUBSCRIBE:
		if (pdu[0] == ATT_WRITE_RSP) {
			hog->next++;
			subscribe_next(hog);
		}
		break;
	default:
		break;
	}
}

int bgpd_hog_attach(struct bgpd_hog *hog, int att, const char *name)
{
	bgpd_hog_close(hog);
	hog->att = att;
	hog->count = hog->next = 0;
	hog->service_start = hog->service_end = 0;
	snprintf(hog->name, sizeof(hog->name), "%s", name);
	hog->state = BGPD_HOG_SERVICES;
	return request_range(hog, ATT_READ_BY_GROUP_REQ, 1, 0xffff,
			     UUID_PRIMARY);
}

int bgpd_hog_connect(struct bgpd_hog *hog, const uint8_t addr[BGPD_ADDR_SIZE],
		     const char *name)
{
	struct sockaddr_l2cap sa = {.family = AF_BLUETOOTH_FAMILY,
				    .cid = ATT_CID,
				    .addr_type = addr[6]};
	struct bt_security_option security = {.level = BT_SECURITY_MEDIUM};
	int fd;

	if (addr[6] == BGPD_ADDR_BREDR || bgpd_hog_active(hog))
		return -1;
	memcpy(sa.addr, addr, 6);
	fd = socket(AF_BLUETOOTH_FAMILY,
		    SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    BTPROTO_L2CAP);
	if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    setsockopt(fd, SOL_BLUETOOTH_LEVEL, BT_SECURITY_OPTION, &security,
		       sizeof(security)) < 0 ||
	    (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
	     errno != EINPROGRESS)) {
		say(hog, "Cannot connect: %s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	hog->att = fd;
	snprintf(hog->name, sizeof(hog->name), "%s", name);
	hog->state = BGPD_HOG_CONNECTING;
	hog->deadline = now_ms() + CONNECT_TIMEOUT_MS;
	say(hog, "Connecting to %s...", name);
	return 0;
}

static void check_connected(struct bgpd_hog *hog)
{
	struct pollfd p = {.fd = hog->att, .events = POLLOUT};
	int error = 0;
	socklen_t length = sizeof(error);

	if (poll(&p, 1, 0) != 1)
		return;
	if (getsockopt(hog->att, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
	    error != 0 ||
	    ((unsigned int)p.revents & (unsigned int)POLLERR) != 0 ||
	    ((unsigned int)p.revents & (unsigned int)POLLHUP) != 0) {
		say(hog, "Connection failed: %s",
		    error != 0 ? strerror(error) : "closed");
		bgpd_hog_close(hog);
		return;
	}
	hog->state = BGPD_HOG_SERVICES;
	hog->count = hog->next = 0;
	hog->service_start = hog->service_end = 0;
	say(hog, "Connected; reading the services...");
	request_range(hog, ATT_READ_BY_GROUP_REQ, 1, 0xffff, UUID_PRIMARY);
}

static void drain_uhid(struct bgpd_hog *hog)
{
	struct uhid_event ev;

	/* Requests of the input device are not answered; just keep it quiet. */
	while (hog->uhid >= 0 && read(hog->uhid, &ev, sizeof(ev)) > 0)
		;
}

void bgpd_hog_poll(struct bgpd_hog *hog)
{
	uint8_t pdu[PACKET_SIZE];

	if (hog->state == BGPD_HOG_IDLE)
		return;
	if (hog->state == BGPD_HOG_CONNECTING)
		check_connected(hog);
	for (int i = 0;
	     i < 32 && hog->att >= 0 && hog->state != BGPD_HOG_CONNECTING;
	     i++) {
		ssize_t n = read(hog->att, pdu, sizeof(pdu));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			say(hog, "%s disconnected", hog->name);
			bgpd_hog_close(hog);
			return;
		}
		on_pdu(hog, pdu, (size_t)n);
	}
	drain_uhid(hog);
	if (hog->state != BGPD_HOG_IDLE && hog->state != BGPD_HOG_RUNNING &&
	    hog->deadline != 0 && now_ms() > hog->deadline)
		fail(hog, "The device did not answer");
}
