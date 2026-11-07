/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "mgmt.h"

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

#define BR_EDR (1U << 7U)
#define LE (1U << 9U)

static int peer;
static struct bgpd_mgmt *m;
static uint8_t packet[65541];
static const uint8_t remote[BGPD_ADDR_SIZE] = {0x55, 0x44, 0x33,	   0x22,
					       0x11, 0x00, BGPD_ADDR_BREDR};

/* Reads the next command of the program and returns its payload size. */
static size_t expect(uint16_t op)
{
	struct pollfd p = {.fd = peer, .events = POLLIN};
	ssize_t n;

	if (poll(&p, 1, 1000) != 1) {
		CHECK(!"command expected");
		return 0;
	}
	n = read(peer, packet, sizeof(packet));
	CHECK(n >= 6 && bgpd_get16(packet) == op);
	CHECK((size_t)n == 6 + (size_t)bgpd_get16(packet + 4));
	CHECK(bgpd_get16(packet + 2) ==
	      (op == BGPD_OP_INDEX_LIST ? BGPD_NO_INDEX : m->index));
	return n >= 6 ? (size_t)n - 6 : 0;
}

static void event(uint16_t type, uint16_t index, const void *data, size_t size)
{
	uint8_t frame[65541];

	bgpd_put16(frame, type);
	bgpd_put16(frame + 2, index);
	bgpd_put16(frame + 4, (uint16_t)size);
	if (size > 0)
		memcpy(frame + 6, data, size);
	CHECK(write(peer, frame, size + 6) == (ssize_t)(size + 6));
	bgpd_mgmt_poll(m);
}

static void reply(uint16_t op, uint8_t code, const void *data, size_t size)
{
	uint8_t body[65535];

	bgpd_put16(body, op);
	body[2] = code;
	if (size > 0)
		memcpy(body + 3, data, size);
	event(1, op == BGPD_OP_INDEX_LIST ? BGPD_NO_INDEX : m->index, body,
	      size + 3);
}

static void init(bool powered)
{
	uint8_t list[4] = {1, 0, 0, 0};
	uint8_t info[280] = {0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
	uint32_t bits = 1U | (1U << 4U) | (1U << 6U) | BR_EDR | LE;

	CHECK(bgpd_mgmt_attach(m, m->fd) == 0);
	CHECK(expect(BGPD_OP_INDEX_LIST) == 0);
	reply(BGPD_OP_INDEX_LIST, 0, list, sizeof(list));
	CHECK(expect(BGPD_OP_INFO) == 0);
	bgpd_put32(info + 9, bits);
	bgpd_put32(info + 13, powered ? bits : bits & ~1U);
	memcpy(info + 20, "Test controller", 16);
	reply(BGPD_OP_INFO, 0, info, sizeof(info));
	CHECK(expect(BGPD_OP_LOAD_LINK_KEYS) == 3);
	reply(BGPD_OP_LOAD_LINK_KEYS, 0, NULL, 0);
	CHECK(expect(BGPD_OP_LOAD_LTKS) == 2);
	reply(BGPD_OP_LOAD_LTKS, 0, NULL, 0);
	CHECK(expect(BGPD_OP_LOAD_IRKS) == 2);
	reply(BGPD_OP_LOAD_IRKS, 0, NULL, 0);
	if (powered) {
		uint8_t connections[2] = {0, 0};

		CHECK(expect(BGPD_OP_CONNECTIONS) == 0);
		reply(BGPD_OP_CONNECTIONS, 0, connections, 2);
	}
	CHECK(m->ready && m->scan_type == 7 && m->devices.count == 0);
	CHECK(strcmp(m->controller_name, "Test controller") == 0);
	CHECK(strcmp(m->controller_address, "01:02:03:04:05:06") == 0);
}

static void test_scan(void)
{
	uint8_t found[14 + 9] = {0};
	uint8_t discovering[2] = {7, 1};
	struct bgpd_device *d;

	bgpd_mgmt_scan(m);
	CHECK(expect(BGPD_OP_START_DISCOVERY) == 1 && packet[6] == 7);
	reply(BGPD_OP_START_DISCOVERY, 0, &m->scan_type, 1);
	CHECK(m->scanning && m->own_scan);
	event(0x13, m->index, discovering, sizeof(discovering));

	memcpy(found, remote, BGPD_ADDR_SIZE);
	found[7] = (uint8_t)-45;
	bgpd_put16(found + 12, 9);
	found[14] = 8; /* length of the field */
	found[15] = 9; /* complete name */
	memcpy(found + 16, (const uint8_t[]){72, 101, 97, 100, 115, 101, 116},
	       7);
	event(0x12, m->index, found, sizeof(found));
	d = bgpd_devices_get(&m->devices, remote);
	CHECK(d != NULL && strcmp(d->name, "Headset") == 0);
	CHECK(d != NULL && d->has_rssi && d->rssi == -45);

	bgpd_mgmt_scan(m);
	CHECK(expect(BGPD_OP_STOP_DISCOVERY) == 1);
	reply(BGPD_OP_STOP_DISCOVERY, 0, &m->scan_type, 1);
	CHECK(!m->scanning && !m->own_scan);
}

static void test_power(void)
{
	uint8_t settings[4];

	bgpd_mgmt_toggle_power(m);
	CHECK(expect(BGPD_OP_POWER) == 1 && packet[6] == 0);
	bgpd_put32(settings, m->settings & ~1U);
	reply(BGPD_OP_POWER, 0, settings, sizeof(settings));
	CHECK((m->settings & 1U) == 0);
	bgpd_mgmt_scan(m);
	CHECK(strstr(m->status, "on first") != NULL);
	bgpd_mgmt_toggle_power(m);
	CHECK(expect(BGPD_OP_POWER) == 1 && packet[6] == 1);
	bgpd_put32(settings, m->settings | 1U);
	reply(BGPD_OP_POWER, 0, settings, sizeof(settings));
}

static void test_keys(void)
{
	uint8_t key[26] = {1};
	struct bgpd_device *d;

	memcpy(key + 1, remote, BGPD_ADDR_SIZE);
	key[1 + 7] = 4;
	event(0x09, m->index, key, sizeof(key));
	d = bgpd_devices_get(&m->devices, remote);
	CHECK(d != NULL && d->paired && d->saved && m->store.count == 1);
	event(0x0b, m->index, (uint8_t[13]){0x55, 0x44, 0x33, 0x22, 0x11}, 13);
	CHECK(d != NULL && d->connected);
	event(0x0c, m->index, remote, sizeof(remote));
	CHECK(d != NULL && !d->connected && d->paired);
	event(0x16, m->index, remote, sizeof(remote));
	CHECK(d != NULL && !d->paired && !d->saved && m->store.count == 0);
}

static void request(uint16_t type, uint8_t flag, uint32_t value)
{
	uint8_t data[12] = {0};

	memcpy(data, remote, BGPD_ADDR_SIZE);
	data[7] = flag;
	bgpd_put32(data + 8, value);
	event(type, m->index, data, type == 0x0f ? 12 : type == 0x0e ? 8 : 7);
}

static void test_pairing(void)
{
	struct bgpd_device *d = bgpd_devices_get(&m->devices, remote);
	uint8_t other[BGPD_ADDR_SIZE] = {9, 9, 9, 9, 9, 9, BGPD_ADDR_BREDR};
	uint8_t done[BGPD_ADDR_SIZE + 1];

	CHECK(d != NULL);
	if (d == NULL)
		return;
	bgpd_mgmt_pair(m, d);
	CHECK(expect(BGPD_OP_PAIR) == 8 && packet[6 + 7] == 4);
	CHECK(m->pairing);

	/* A request from another device is refused. */
	event(0x0f, m->index, (uint8_t[12]){9, 9, 9, 9, 9, 9, 0}, 12);
	CHECK(expect(BGPD_OP_CONFIRM_NEGATIVE) == BGPD_ADDR_SIZE);
	CHECK(m->prompt_kind == BGPD_PROMPT_NONE);

	request(0x0f, 0, 123456);
	CHECK(m->prompt_kind == BGPD_PROMPT_CONFIRM);
	CHECK(strstr(m->prompt, "123456") != NULL);
	bgpd_mgmt_reply(m, true);
	CHECK(expect(BGPD_OP_CONFIRM) == BGPD_ADDR_SIZE);
	CHECK(m->prompt_kind == BGPD_PROMPT_NONE);

	request(0x10, 0, 0);
	snprintf(m->input, sizeof(m->input), "042");
	bgpd_mgmt_reply(m, true);
	CHECK(expect(BGPD_OP_PASSKEY) == 11 &&
	      bgpd_get32(packet + 6 + 7) == 42);

	request(0x0e, 1, 0);
	snprintf(m->input, sizeof(m->input), "1234");
	bgpd_mgmt_reply(m, true); /* a secure PIN needs 16 characters */
	CHECK(m->prompt_kind == BGPD_PROMPT_PIN);
	snprintf(m->input, sizeof(m->input), "1234567890123456");
	bgpd_mgmt_reply(m, true);
	CHECK(expect(BGPD_OP_PIN_REPLY) == 24 && packet[6 + 7] == 16);

	request(0x0e, 0, 0);
	bgpd_mgmt_reply(m, false);
	CHECK(expect(BGPD_OP_PIN_NEGATIVE) == BGPD_ADDR_SIZE);

	memcpy(done, remote, BGPD_ADDR_SIZE);
	done[BGPD_ADDR_SIZE] = 0;
	reply(BGPD_OP_PAIR, 0, done, BGPD_ADDR_SIZE);
	CHECK(!m->pairing && d->paired);
	CHECK(strstr(m->status, "Pairing successful") != NULL);
	CHECK(bgpd_devices_get(&m->devices, other) != NULL);
}

static void test_cancel_and_failure(void)
{
	struct bgpd_device *d = bgpd_devices_get(&m->devices, remote);

	CHECK(d != NULL);
	if (d == NULL)
		return;
	d->paired = false;
	bgpd_mgmt_pair(m, d);
	CHECK(expect(BGPD_OP_PAIR) == 8);
	bgpd_mgmt_cancel_pair(m);
	CHECK(expect(BGPD_OP_CANCEL_PAIR) == BGPD_ADDR_SIZE);
	reply(BGPD_OP_PAIR, 0x10, NULL, 0);
	CHECK(!m->pairing && strstr(m->status, "Cancelled") != NULL);

	d->paired = true;
	bgpd_mgmt_pair(m, d);
	CHECK(strcmp(m->status, "Already paired") == 0);
}

static void test_unpair_and_disconnect(void)
{
	struct bgpd_device *d = bgpd_devices_get(&m->devices, remote);
	uint8_t key[26] = {1};

	CHECK(d != NULL);
	if (d == NULL)
		return;
	memcpy(key + 1, remote, BGPD_ADDR_SIZE);
	key[1 + 7] = 4;
	event(0x09, m->index, key, sizeof(key));
	d->connected = true;
	bgpd_mgmt_disconnect(m, d);
	CHECK(expect(BGPD_OP_DISCONNECT) == BGPD_ADDR_SIZE);
	reply(BGPD_OP_DISCONNECT, 0, remote, BGPD_ADDR_SIZE);
	CHECK(!d->connected);
	bgpd_mgmt_unpair(m, d);
	CHECK(expect(BGPD_OP_UNPAIR) == 8 && packet[6 + 7] == 1);
	reply(BGPD_OP_UNPAIR, 0, remote, BGPD_ADDR_SIZE);
	CHECK(!d->paired && !d->saved && m->store.count == 0);
}

static void test_pair_while_scanning(void)
{
	struct bgpd_device *d = bgpd_devices_get(&m->devices, remote);

	CHECK(d != NULL);
	if (d == NULL)
		return;
	d->paired = false;
	bgpd_mgmt_scan(m);
	CHECK(expect(BGPD_OP_START_DISCOVERY) == 1);
	reply(BGPD_OP_START_DISCOVERY, 0, &m->scan_type, 1);
	bgpd_mgmt_pair(m, d);
	CHECK(expect(BGPD_OP_STOP_DISCOVERY) == 1);
	reply(BGPD_OP_STOP_DISCOVERY, 0, &m->scan_type, 1);
	CHECK(expect(BGPD_OP_PAIR) == 8);
	bgpd_mgmt_cancel_pair(m);
	CHECK(expect(BGPD_OP_CANCEL_PAIR) == BGPD_ADDR_SIZE);
	reply(BGPD_OP_PAIR, 0x10, NULL, 0);
}

static void test_trace(const char *dir)
{
	char path[300], text[8192] = {0};
	FILE *f;

	snprintf(path, sizeof(path), "%s/trace.log", dir);
	f = fopen(path, "r");
	CHECK(f != NULL);
	if (f == NULL)
		return;
	CHECK(fread(text, 1, sizeof(text) - 1, f) > 0);
	fclose(f);
	CHECK(strstr(text, " ms > 0x0003 hci65535 (0 bytes)") != NULL);
	CHECK(strstr(text, " ms < 0x0001") != NULL);
	CHECK(strstr(text, "<hidden>") != NULL); /* keys are not logged */
	unlink(path);
}

static void test_failures(void)
{
	uint8_t other[2] = {0, 0};

	bgpd_mgmt_scan(m);
	CHECK(expect(BGPD_OP_START_DISCOVERY) == 1);
	reply(BGPD_OP_START_DISCOVERY, 0x0a, NULL, 0);
	CHECK(strstr(m->status, "Busy") != NULL && m->command == 0);
	event(5, m->index, NULL, 0);
	CHECK(!m->ready);
	CHECK(bgpd_get16(other) == 0);
}

int main(void)
{
	char dir[] = "/tmp/bgpd-mgmt-XXXXXX";
	char file[300];
	int fds[2];

	if (mkdtemp(dir) == NULL ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) < 0)
		return 1;
	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return 1;
	bgpd_mgmt_init(m, BGPD_NO_INDEX, dir);
	m->fd = fds[0];
	snprintf(file, sizeof(file), "%s/trace.log", dir);
	CHECK(bgpd_mgmt_trace(m, file) == 0);
	peer = fds[1];
	init(true);
	test_scan();
	test_power();
	test_keys();
	test_pairing();
	test_cancel_and_failure();
	test_unpair_and_disconnect();
	test_pair_while_scanning();
	test_failures();
	bgpd_mgmt_close(m);
	test_trace(dir);
	close(peer);
	snprintf(file, sizeof(file), "%s/010203040506.keys", dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/.lock", dir);
	unlink(file);
	rmdir(dir);
	free(m);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
