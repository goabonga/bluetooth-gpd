/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "avrcp.h"

#include <errno.h>
#include <poll.h>
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
#define AVCTP_PSM 23

#define CONNECT_TIMEOUT_MS 15000U
#define ANSWER_TIMEOUT_MS 4000U

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

/* AVCTP header: transaction label, packet type, command or response. */
#define AVCTP_RESPONSE 0x02U
#define PID_AVRCP 0x110eU
#define HEADER_SIZE 3U
/* AV/C: command types and responses, panel subunit, vendor opcode. */
#define CTYPE_CONTROL 0x00U
#define CTYPE_NOTIFY 0x03U
#define CTYPE_NOT_IMPLEMENTED 0x08U
#define CTYPE_ACCEPTED 0x09U
#define CTYPE_REJECTED 0x0aU
#define CTYPE_CHANGED 0x0dU
#define CTYPE_INTERIM 0x0fU
#define SUBUNIT_PANEL 0x48U
#define OPCODE_VENDOR 0x00U
/* Bluetooth SIG company id, then the AVRCP PDU and its parameters. */
#define PDU_REGISTER_NOTIFICATION 0x31U
#define PDU_SET_ABSOLUTE_VOLUME 0x50U
#define EVENT_VOLUME_CHANGED 0x0dU
#define AVC_HEADER 3U	 /* ctype, subunit, opcode */
#define VENDOR_PREFIX 7U /* company 3, pdu 1, packet type 1, length 2 */
#define MAX_PARAMS 8U
#define VOLUME_MAX 127U

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int open_l2cap(const uint8_t addr[BGPD_ADDR_SIZE])
{
	struct sockaddr_l2cap sa = {.family = AF_BLUETOOTH_FAMILY,
				    .psm = AVCTP_PSM};
	struct bt_security_option security = {.level = BT_SECURITY_MEDIUM};
	int fd = socket(AF_BLUETOOTH_FAMILY,
			SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
			BTPROTO_L2CAP);

	memcpy(sa.addr, addr, 6);
	if (fd >= 0 && (setsockopt(fd, SOL_BLUETOOTH_LEVEL, BT_SECURITY_OPTION,
				   &security, sizeof(security)) < 0 ||
			(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
			 errno != EINPROGRESS))) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

void bgpd_avrcp_init(struct bgpd_avrcp *a)
{
	memset(a, 0, sizeof(*a));
	a->fd = -1;
	a->volume = -1;
	a->open_channel = open_l2cap;
}

bool bgpd_avrcp_active(const struct bgpd_avrcp *a)
{
	return a->state != BGPD_AVRCP_IDLE;
}

void bgpd_avrcp_close(struct bgpd_avrcp *a)
{
	if (a->fd >= 0)
		close(a->fd);
	a->fd = -1;
	a->state = BGPD_AVRCP_IDLE;
	if (a->supported || a->volume >= 0)
		a->changed = true;
	a->supported = false;
	a->volume = -1;
}

static unsigned int to_device(unsigned int percent)
{
	return (percent * VOLUME_MAX + 50U) / 100U;
}

static int to_percent(unsigned int value)
{
	return (int)((value * 100U + VOLUME_MAX / 2U) / VOLUME_MAX);
}

/* Sends a vendor dependent command with a one or five byte parameter. */
static int send_command(struct bgpd_avrcp *a, uint8_t ctype, uint8_t pdu,
			const uint8_t *params, size_t length)
{
	uint8_t frame[HEADER_SIZE + AVC_HEADER + VENDOR_PREFIX + MAX_PARAMS];
	size_t size = 0;
	ssize_t n;

	a->label = (uint8_t)((a->label + 1U) & 0x0fU);
	frame[size++] = (uint8_t)(a->label << 4U);
	frame[size++] = PID_AVRCP >> 8U;
	frame[size++] = PID_AVRCP & 0xffU;
	frame[size++] = ctype;
	frame[size++] = SUBUNIT_PANEL;
	frame[size++] = OPCODE_VENDOR;
	frame[size++] = 0x00; /* Bluetooth SIG */
	frame[size++] = 0x19;
	frame[size++] = 0x58;
	frame[size++] = pdu;
	frame[size++] = 0x00; /* single packet */
	frame[size++] = (uint8_t)(length >> 8U);
	frame[size++] = (uint8_t)length;
	if (length > sizeof(frame) - size)
		return -1;
	memcpy(frame + size, params, length);
	size += length;
	do {
		n = write(a->fd, frame, size);
	} while (n < 0 && errno == EINTR);
	return n == (ssize_t)size ? 0 : -1;
}

/* Asks the device to tell when its volume changes. */
static int register_volume(struct bgpd_avrcp *a)
{
	const uint8_t params[5] = {EVENT_VOLUME_CHANGED, 0, 0, 0, 0};

	a->deadline = now_ms() + ANSWER_TIMEOUT_MS;
	return send_command(a, CTYPE_NOTIFY, PDU_REGISTER_NOTIFICATION, params,
			    sizeof(params));
}

int bgpd_avrcp_connect(struct bgpd_avrcp *a, const uint8_t addr[BGPD_ADDR_SIZE])
{
	int fd;

	if (addr[6] != BGPD_ADDR_BREDR || bgpd_avrcp_active(a)) {
		errno = EINVAL;
		return -1;
	}
	fd = a->open_channel(addr);
	if (fd < 0)
		return -1;
	a->fd = fd;
	memcpy(a->addr, addr, BGPD_ADDR_SIZE);
	a->state = BGPD_AVRCP_CONNECTING;
	a->deadline = now_ms() + CONNECT_TIMEOUT_MS;
	return 0;
}

static void set_volume_known(struct bgpd_avrcp *a, unsigned int value)
{
	int percent = to_percent(value & VOLUME_MAX);

	if (!a->supported || a->volume != percent)
		a->changed = true;
	a->supported = true;
	a->volume = percent;
	a->deadline = 0;
}

/* The device cannot do it: the volume is applied in software. */
static void unsupported(struct bgpd_avrcp *a)
{
	if (a->supported)
		a->changed = true;
	a->supported = false;
	a->volume = -1;
	a->deadline = 0;
}

/* A response to one of our commands. */
static void on_response(struct bgpd_avrcp *a, uint8_t ctype, uint8_t pdu,
			const uint8_t *params, size_t length)
{
	if (pdu == PDU_REGISTER_NOTIFICATION) {
		if (ctype == CTYPE_NOT_IMPLEMENTED || ctype == CTYPE_REJECTED) {
			unsupported(a);
		} else if ((ctype == CTYPE_INTERIM || ctype == CTYPE_CHANGED) &&
			   length >= 2 && params[0] == EVENT_VOLUME_CHANGED) {
			set_volume_known(a, params[1]);
			/* A change ends the notification: ask for the next. */
			if (ctype == CTYPE_CHANGED)
				register_volume(a);
		}
	} else if (pdu == PDU_SET_ABSOLUTE_VOLUME) {
		if (ctype == CTYPE_ACCEPTED && length >= 1)
			set_volume_known(a, params[0]);
		else if (ctype == CTYPE_NOT_IMPLEMENTED)
			unsupported(a);
	}
}

/* A command of the device: nothing is implemented, say so. */
static void on_command(struct bgpd_avrcp *a, const uint8_t *packet, size_t size)
{
	uint8_t reply[64];
	ssize_t n;

	if (size > sizeof(reply) || size < HEADER_SIZE + AVC_HEADER)
		return;
	memcpy(reply, packet, size);
	reply[0] = (uint8_t)(reply[0] | AVCTP_RESPONSE);
	reply[HEADER_SIZE] = CTYPE_NOT_IMPLEMENTED;
	n = write(a->fd, reply, size);
	(void)n;
}

static void on_packet(struct bgpd_avrcp *a, const uint8_t *p, size_t size)
{
	unsigned int pid;

	if (size < HEADER_SIZE + AVC_HEADER)
		return;
	pid = (unsigned int)(p[1] << 8U) | p[2];
	if (pid != PID_AVRCP)
		return;
	if (((unsigned int)p[0] & AVCTP_RESPONSE) == 0) {
		on_command(a, p, size);
		return;
	}
	if (p[HEADER_SIZE + 2] != OPCODE_VENDOR ||
	    size < HEADER_SIZE + AVC_HEADER + VENDOR_PREFIX)
		return;
	{
		const uint8_t *vendor = p + HEADER_SIZE + AVC_HEADER;
		size_t length = (size_t)(vendor[5] << 8U) | vendor[6];

		if (size < HEADER_SIZE + AVC_HEADER + VENDOR_PREFIX + length)
			return;
		on_response(a, p[HEADER_SIZE] & 0x0fU, vendor[3],
			    vendor + VENDOR_PREFIX, length);
	}
}

int bgpd_avrcp_set_volume(struct bgpd_avrcp *a, unsigned int percent)
{
	uint8_t value;

	if (a->state != BGPD_AVRCP_ACTIVE || !a->supported)
		return -1;
	value = (uint8_t)to_device(percent > 100U ? 100U : percent);
	return send_command(a, CTYPE_CONTROL, PDU_SET_ABSOLUTE_VOLUME, &value,
			    1);
}

void bgpd_avrcp_poll(struct bgpd_avrcp *a)
{
	uint8_t packet[512];

	if (a->state == BGPD_AVRCP_IDLE)
		return;
	if (a->state == BGPD_AVRCP_CONNECTING) {
		struct pollfd p = {.fd = a->fd, .events = POLLOUT};
		int error = 0;
		socklen_t length = sizeof(error);

		if (poll(&p, 1, 0) == 1) {
			if (getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &error,
				       &length) < 0 ||
			    error != 0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLERR) !=
				0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLHUP) !=
				0) {
				bgpd_avrcp_close(a);
				return;
			}
			a->state = BGPD_AVRCP_ACTIVE;
			if (register_volume(a) < 0) {
				bgpd_avrcp_close(a);
				return;
			}
		} else if (now_ms() > a->deadline) {
			bgpd_avrcp_close(a);
			return;
		}
	}
	for (int i = 0; i < 16 && a->fd >= 0 && a->state == BGPD_AVRCP_ACTIVE;
	     i++) {
		ssize_t n = read(a->fd, packet, sizeof(packet));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			bgpd_avrcp_close(a);
			return;
		}
		on_packet(a, packet, (size_t)n);
	}
	/* No answer to the registration: the device has no absolute volume. */
	if (a->state == BGPD_AVRCP_ACTIVE && a->deadline != 0 &&
	    now_ms() > a->deadline)
		unsupported(a);
}
