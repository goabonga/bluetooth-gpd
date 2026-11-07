/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "hog.h"

#include <fcntl.h>
#include <linux/uhid.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static int peer;
static struct bgpd_hog hog;
static uint8_t request[512];

/* The next ATT request of the program, checked for its opcode. */
static void expect(uint8_t opcode)
{
	struct pollfd p = {.fd = peer, .events = POLLIN};
	ssize_t n;

	if (poll(&p, 1, 1000) != 1) {
		CHECK(!"request expected");
		return;
	}
	n = read(peer, request, sizeof(request));
	CHECK(n > 0 && request[0] == opcode);
}

/* What the device answers, handled at once. */
static void answer(const uint8_t *pdu, size_t size)
{
	CHECK(write(peer, pdu, size) == (ssize_t)size);
	bgpd_hog_poll(&hog);
}

static void not_found(uint8_t opcode, uint16_t handle)
{
	const uint8_t pdu[] = {1, opcode, (uint8_t)(handle & 0xffU),
			       (uint8_t)(handle >> 8U), 0x0a};

	answer(pdu, sizeof(pdu));
}

static void discover(void)
{
	/* One service of no interest, then the HID service 0x0010-0x0020. */
	const uint8_t first[] = {0x11, 6, 1, 0, 5, 0, 0x00, 0x18};
	const uint8_t second[] = {0x11, 6, 0x10, 0, 0x20, 0, 0x12, 0x18};
	/* Report map 0x0012 and an input report 0x0014 that notifies. */
	const uint8_t chars[] = {0x09, 7,    0x11, 0,	 0x02, 0x12,
				 0,    0x4b, 0x2a, 0x13, 0,    0x12,
				 0x14, 0,    0x4d, 0x2a};
	const uint8_t info[] = {0x05, 1,    0x15, 0,	0x02,
				0x29, 0x16, 0,	  0x08, 0x29};
	const uint8_t map[] = {0x05, 0x01, 0x09, 0x06, 0xc0};
	const uint8_t reference[] = {0x0b, 1, 1};

	CHECK(hog.state == BGPD_HOG_SERVICES);
	expect(0x10);
	answer(first, sizeof(first));
	expect(0x10);
	CHECK(request[1] == 6 && request[2] == 0); /* starts after 0x0005 */
	answer(second, sizeof(second));
	expect(0x08);
	CHECK(request[1] == 0x10 && request[5] == 0x03 && request[6] == 0x28);
	answer(chars, sizeof(chars));
	expect(0x08);
	CHECK(request[1] == 0x14); /* continues after the last declaration */
	not_found(0x08, 0x14);
	expect(0x04);
	answer(info, sizeof(info));
	expect(0x04);
	CHECK(request[1] == 0x17);
	not_found(0x04, 0x17);
	expect(0x0a);
	CHECK(request[1] == 0x12);
	answer((uint8_t[]){0x0b, 0x05, 0x01, 0x09, 0x06, 0xc0}, 6);
	(void)map;
	expect(0x0a);
	CHECK(request[1] == 0x16); /* report reference of the input */
	answer(reference, sizeof(reference));
	expect(0x12);
	CHECK(request[1] == 0x15 && request[3] == 1 && request[4] == 0);
	answer((uint8_t[]){0x13}, 1);
}

static void test_session(const char *uhid_path)
{
	struct uhid_event ev;
	int fd;
	ssize_t n;

	fd = open(uhid_path, O_RDONLY);
	CHECK(fd >= 0);
	CHECK(hog.state == BGPD_HOG_RUNNING);
	CHECK(strstr(hog.status, "input device") != NULL);
	n = read(fd, &ev, sizeof(ev));
	CHECK(n == (ssize_t)sizeof(ev) && ev.type == UHID_CREATE2);
	CHECK(strcmp((char *)ev.u.create2.name, "Flipper") == 0);
	CHECK(ev.u.create2.rd_size == 5 && ev.u.create2.rd_data[3] == 0x06);

	/* A key press: the report id leads the data. */
	answer((uint8_t[]){0x1b, 0x14, 0x00, 0x00, 0x00, 0x04}, 6);
	n = read(fd, &ev, sizeof(ev));
	CHECK(n == (ssize_t)sizeof(ev) && ev.type == UHID_INPUT2);
	CHECK(ev.u.input2.size == 4 && ev.u.input2.data[0] == 1);
	CHECK(ev.u.input2.data[3] == 0x04);
	close(fd);
}

static void test_no_hid_service(void)
{
	int fds[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	peer = fds[1];
	CHECK(bgpd_hog_attach(&hog, fds[0], "Mouse") == 0);
	expect(0x10);
	answer((uint8_t[]){0x11, 6, 1, 0, 5, 0, 0x00, 0x18}, 8);
	expect(0x10);
	not_found(0x10, 6);
	CHECK(!bgpd_hog_active(&hog));
	CHECK(strstr(hog.status, "no HID service") != NULL);
	close(peer);
}

static void test_disconnect(void)
{
	int fds[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
	peer = fds[1];
	CHECK(bgpd_hog_attach(&hog, fds[0], "Pad") == 0);
	expect(0x10);
	close(peer);
	bgpd_hog_poll(&hog);
	CHECK(!bgpd_hog_active(&hog));
	CHECK(strstr(hog.status, "Pad disconnected") != NULL);
}

int main(void)
{
	char path[] = "/tmp/bgpd-uhid-XXXXXX";
	int fds[2], fd = mkstemp(path);

	if (fd < 0 ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) < 0)
		return 1;
	close(fd);
	bgpd_hog_init(&hog);
	hog.uhid_path = path;
	peer = fds[1];
	CHECK(bgpd_hog_attach(&hog, fds[0], "Flipper") == 0);
	discover();
	test_session(path);
	bgpd_hog_close(&hog);
	close(peer);
	unlink(path);
	test_no_hid_service();
	test_disconnect();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
