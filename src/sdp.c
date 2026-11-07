/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "sdp.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Kernel values, so that no BlueZ header is needed. */
#define AF_BLUETOOTH_FAMILY 31
#define BTPROTO_L2CAP 0
#define SDP_PSM 1

#define TIMEOUT_MS 10000U
#define PDU_SEARCH_ATTRIBUTE_REQUEST 0x06U
#define PDU_SEARCH_ATTRIBUTE_RESPONSE 0x07U
#define PDU_ERROR_RESPONSE 0x01U
#define PDU_HEADER 5U
#define UUID_RFCOMM 0x0003U
#define MAX_DEPTH 8U

/* Data element types of the SDP specification. */
#define TYPE_UINT 1U
#define TYPE_UUID 3U
#define TYPE_SEQUENCE 6U
#define TYPE_ALTERNATIVE 7U

struct sockaddr_l2cap {
	sa_family_t family;
	uint16_t psm;
	uint8_t addr[6];
	uint16_t cid;
	uint8_t addr_type;
};

/* A big-endian 16-bit number. */
static unsigned int be16(const uint8_t *p)
{
	return 256U * p[0] + p[1];
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int open_l2cap(const uint8_t addr[BGPD_ADDR_SIZE])
{
	struct sockaddr_l2cap sa = {.family = AF_BLUETOOTH_FAMILY,
				    .psm = SDP_PSM};
	int fd = socket(AF_BLUETOOTH_FAMILY,
			SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
			BTPROTO_L2CAP);

	memcpy(sa.addr, addr, 6);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
	    errno != EINPROGRESS) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

void bgpd_sdp_init(struct bgpd_sdp *q)
{
	memset(q, 0, sizeof(*q));
	q->fd = -1;
	q->open_channel = open_l2cap;
}

void bgpd_sdp_close(struct bgpd_sdp *q)
{
	if (q->fd >= 0)
		close(q->fd);
	q->fd = -1;
	if (q->state != BGPD_SDP_FOUND && q->state != BGPD_SDP_FAILED)
		q->state = BGPD_SDP_IDLE;
}

/* ---- data elements ---- */

/*
 * Reads the header of the element at p: its type, the size of its header
 * and of its data. Returns false if it does not fit in size bytes.
 */
static bool element(const uint8_t *p, size_t size, unsigned int *type,
		    size_t *header, size_t *length)
{
	unsigned int index;

	if (size < 1)
		return false;
	*type = p[0] >> 3U;
	index = p[0] & 7U;
	if (index <= 4) {
		static const size_t sizes[] = {1, 2, 4, 8, 16};

		*header = 1;
		*length = *type == 0 ? 0 : sizes[index];
	} else {
		size_t extra = index == 5 ? 1 : index == 6 ? 2 : 4;

		if (size < 1 + extra)
			return false;
		*header = 1 + extra;
		*length = 0;
		for (size_t i = 0; i < extra; i++)
			*length = (*length << 8U) | p[1 + i];
	}
	return *header + *length <= size;
}

/* The 16-bit UUID an element stands for, or -1. */
static long uuid16(const uint8_t *data, size_t length)
{
	static const uint8_t base[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
					 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};

	if (length == 2)
		return (long)be16(data);
	if (length == 4 && data[0] == 0 && data[1] == 0)
		return (long)be16(data + 2);
	if (length == 16 && data[0] == 0 && data[1] == 0 &&
	    memcmp(data + 4, base, sizeof(base)) == 0)
		return (long)be16(data + 2);
	return -1;
}

/* Looks for a UUID 0x0003 followed by a one byte integer, in sequences. */
static int scan(const uint8_t *p, size_t size, unsigned int depth,
		uint8_t *channel)
{
	bool after_rfcomm = false;

	if (depth > MAX_DEPTH)
		return -1;
	while (size > 0) {
		unsigned int type;
		size_t header, length;

		if (!element(p, size, &type, &header, &length))
			return -1;
		if (type == TYPE_SEQUENCE || type == TYPE_ALTERNATIVE) {
			if (scan(p + header, length, depth + 1, channel) == 0)
				return 0;
		} else if (type == TYPE_UUID) {
			after_rfcomm =
			    uuid16(p + header, length) == UUID_RFCOMM;
			p += header + length;
			size -= header + length;
			continue;
		} else if (type == TYPE_UINT && after_rfcomm && length == 1) {
			*channel = p[header];
			return 0;
		}
		after_rfcomm = false;
		p += header + length;
		size -= header + length;
	}
	return -1;
}

int bgpd_sdp_parse(const uint8_t *lists, size_t size, uint8_t *channel)
{
	return scan(lists, size, 0, channel);
}

/* ---- the query ---- */

static int send_request(struct bgpd_sdp *q)
{
	uint8_t pdu[64];
	size_t size = 0, length;
	ssize_t n;

	q->transaction++;
	pdu[size++] = PDU_SEARCH_ATTRIBUTE_REQUEST;
	pdu[size++] = (uint8_t)(q->transaction >> 8U);
	pdu[size++] = (uint8_t)q->transaction;
	size += 2; /* parameter length, set below */
	/* Service search pattern: the UUID of the service. */
	pdu[size++] = 0x35;
	pdu[size++] = 3;
	pdu[size++] = 0x19;
	pdu[size++] = (uint8_t)(q->service >> 8U);
	pdu[size++] = (uint8_t)q->service;
	/* Largest answer, then the attribute ProtocolDescriptorList (0x0004).
	 */
	pdu[size++] = 0x04;
	pdu[size++] = 0x00;
	pdu[size++] = 0x35;
	pdu[size++] = 3;
	pdu[size++] = 0x09;
	pdu[size++] = 0x00;
	pdu[size++] = 0x04;
	pdu[size++] = (uint8_t)q->continuation_size;
	memcpy(pdu + size, q->continuation, q->continuation_size);
	size += q->continuation_size;
	length = size - PDU_HEADER;
	pdu[3] = (uint8_t)(length >> 8U);
	pdu[4] = (uint8_t)length;
	do {
		n = write(q->fd, pdu, size);
	} while (n < 0 && errno == EINTR);
	return n == (ssize_t)size ? 0 : -1;
}

int bgpd_sdp_start(struct bgpd_sdp *q, const uint8_t addr[BGPD_ADDR_SIZE],
		   uint16_t service)
{
	int fd;

	if (addr[6] != BGPD_ADDR_BREDR ||
	    (q->state != BGPD_SDP_IDLE && q->state != BGPD_SDP_FOUND &&
	     q->state != BGPD_SDP_FAILED)) {
		errno = EINVAL;
		return -1;
	}
	fd = q->open_channel(addr);
	if (fd < 0)
		return -1;
	q->fd = fd;
	q->service = service;
	q->used = 0;
	q->continuation_size = 0;
	q->state = BGPD_SDP_CONNECTING;
	q->deadline = now_ms() + TIMEOUT_MS;
	return 0;
}

static void fail(struct bgpd_sdp *q)
{
	if (q->fd >= 0)
		close(q->fd);
	q->fd = -1;
	q->state = BGPD_SDP_FAILED;
}

/* Handles an answer; true when the whole list was received. */
static bool on_response(struct bgpd_sdp *q, const uint8_t *p, size_t size)
{
	size_t count, state;

	if (size < PDU_HEADER + 2 || p[0] != PDU_SEARCH_ATTRIBUTE_RESPONSE ||
	    be16(p + 1) != q->transaction) {
		fail(q);
		return false;
	}
	count = be16(p + 5);
	if (size < PDU_HEADER + 2 + count + 1 ||
	    q->used + count > sizeof(q->response)) {
		fail(q);
		return false;
	}
	memcpy(q->response + q->used, p + PDU_HEADER + 2, count);
	q->used += count;
	state = p[PDU_HEADER + 2 + count];
	if (state == 0)
		return true;
	/* More to come: give the continuation state back. */
	if (state > sizeof(q->continuation) ||
	    size < PDU_HEADER + 2 + count + 1 + state) {
		fail(q);
		return false;
	}
	memcpy(q->continuation, p + PDU_HEADER + 2 + count + 1, state);
	q->continuation_size = state;
	if (send_request(q) < 0)
		fail(q);
	return false;
}

bool bgpd_sdp_poll(struct bgpd_sdp *q)
{
	uint8_t packet[BGPD_SDP_BUFFER];

	if (q->state == BGPD_SDP_IDLE || q->state == BGPD_SDP_FOUND ||
	    q->state == BGPD_SDP_FAILED)
		return q->state != BGPD_SDP_IDLE;
	if (q->state == BGPD_SDP_CONNECTING) {
		struct pollfd p = {.fd = q->fd, .events = POLLOUT};
		int error = 0;
		socklen_t length = sizeof(error);

		if (poll(&p, 1, 0) == 1) {
			if (getsockopt(q->fd, SOL_SOCKET, SO_ERROR, &error,
				       &length) < 0 ||
			    error != 0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLERR) !=
				0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLHUP) !=
				0 ||
			    send_request(q) < 0) {
				fail(q);
				return true;
			}
			q->state = BGPD_SDP_WAITING;
		}
	}
	if (q->state == BGPD_SDP_WAITING) {
		ssize_t n = read(q->fd, packet, sizeof(packet));

		if (n > 0 && on_response(q, packet, (size_t)n)) {
			bool found = bgpd_sdp_parse(q->response, q->used,
						    &q->channel) == 0;

			close(q->fd);
			q->fd = -1;
			q->state = found ? BGPD_SDP_FOUND : BGPD_SDP_FAILED;
			return true;
		}
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR &&
			       errno != EWOULDBLOCK))
			fail(q);
	}
	if (q->state != BGPD_SDP_FAILED && now_ms() > q->deadline)
		fail(q);
	return q->state == BGPD_SDP_FOUND || q->state == BGPD_SDP_FAILED;
}
