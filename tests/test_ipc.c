/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                              \
			failures++;                                            \
		}                                                              \
	} while (0)

static const uint8_t remote[BGPD_ADDR_SIZE] = {
    0x55, 0x44, 0x33, 0x22, 0x11, 0x00, BGPD_ADDR_LE_PUBLIC};

static char snapshot[BGPD_IPC_SNAPSHOT_MAX];

static void fill(struct bgpd_mgmt *m)
{
	struct bgpd_device *d;

	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	m->index = 2;
	m->ready = true;
	m->settings = 1U | (1U << 7U);
	m->scanning = true;
	m->command = 5;
	m->prompt_kind = BGPD_PROMPT_PIN;
	snprintf(m->controller_name, sizeof(m->controller_name), "My\tadapter");
	snprintf(m->controller_address, sizeof(m->controller_address),
		 "00:11:22:33:44:55");
	snprintf(m->status, sizeof(m->status), "Scanning\nnow");
	snprintf(m->warning, sizeof(m->warning), "Mask bluetoothd");
	snprintf(m->prompt, sizeof(m->prompt), "PIN for Keyboard?");
	m->audio.active = true;
	memcpy(m->audio.addr, remote, BGPD_ADDR_SIZE);
	m->audio.state = BGPD_AUDIO_STREAMING;
	m->audio.volume = 35;
	snprintf(m->audio.name, sizeof(m->audio.name), "Head\tset");
	d = bgpd_devices_get(&m->devices, remote);
	if (d == NULL)
		return;
	snprintf(d->name, sizeof(d->name), "Flipper\tZero");
	d->paired = d->connected = d->has_rssi = true;
	d->rssi = -60;
}

static void test_round_trip(void)
{
	struct bgpd_mgmt *a = calloc(1, sizeof(*a));
	struct bgpd_mgmt *b = calloc(1, sizeof(*b));
	size_t n;

	if (a == NULL || b == NULL) {
		free(a);
		free(b);
		CHECK(!"memory");
		return;
	}
	fill(a);
	n = bgpd_ipc_snapshot(a, snapshot, sizeof(snapshot));
	CHECK(n > 0 && strlen(snapshot) == n);
	CHECK(snapshot[n - 1] == '\n');

	bgpd_mgmt_init(b, BGPD_NO_INDEX, NULL);
	snprintf(b->input, sizeof(b->input), "1234");
	CHECK(bgpd_ipc_apply(b, snapshot) == 0);
	CHECK(b->index == 2 && b->ready && b->scanning);
	CHECK(b->settings == a->settings && b->command != 0 && !b->pairing);
	CHECK(b->prompt_kind == BGPD_PROMPT_PIN);
	CHECK(strcmp(b->controller_address, "00:11:22:33:44:55") == 0);
	CHECK(strcmp(b->controller_name, "My adapter") == 0);
	CHECK(strcmp(b->status, "Scanning now") == 0);
	CHECK(strcmp(b->warning, "Mask bluetoothd") == 0);
	CHECK(strcmp(b->prompt, "PIN for Keyboard?") == 0);
	CHECK(strcmp(b->input, "1234") == 0);
	CHECK(b->devices.count == 1);
	CHECK(memcmp(b->devices.items[0].addr, remote, BGPD_ADDR_SIZE) == 0);
	CHECK(strcmp(b->devices.items[0].name, "Flipper Zero") == 0);
	CHECK(b->devices.items[0].paired && b->devices.items[0].connected);
	CHECK(!b->devices.items[0].saved && b->devices.items[0].has_rssi);
	CHECK(b->devices.items[0].rssi == -60);
	CHECK(b->audio.active && b->audio.state == BGPD_AUDIO_STREAMING);
	CHECK(b->audio.volume == 35 && strcmp(b->audio.name, "Head set") == 0);
	CHECK(memcmp(b->audio.addr, remote, BGPD_ADDR_SIZE) == 0);

	/* A new snapshot replaces the devices and the audio device. */
	a->devices.count = 0;
	a->audio.active = false;
	CHECK(bgpd_ipc_snapshot(a, snapshot, sizeof(snapshot)) > 0);
	CHECK(bgpd_ipc_apply(b, snapshot) == 0 && b->devices.count == 0);
	CHECK(!b->audio.active);

	/* It does not fit in a small buffer. */
	fill(a);
	CHECK(bgpd_ipc_snapshot(a, snapshot, 40) == 0);
	free(a);
	free(b);
}

static void test_malformed(void)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	static const char *const bad[] = {
	    "",
	    "end\n",
	    "state\t0\t1\t1\t0\t0\t0\t0\n",
	    "state\t0\t1\t1\t0\t0\t0\t0\nend",
	    "state\t0\t2\t1\t0\t0\t0\t0\nend\n",
	    "state\t0\t1\t1\t0\t0\t0\nend\n",
	    "state\t0\t1\t1\t0\t0\t0\t0\nunknown\nend\n",
	};
	static const char *const bad_audio[] = {
	    "audio\t55443322110001\tplaying\t10\tx\n",
	    "audio\t55443322110001\tready\t101\tx\n",
	    "audio\t5544\tready\t10\tx\n",
	};
	static const char *const bad_devices[] = {
	    "device\t00\t0\t0\tx\n",
	    "device\t55443322110003\t0\t0\tx\n",
	    "device\t55443322110001\t0\t900\tx\n",
	};
	char text[256];

	if (m == NULL) {
		CHECK(!"memory");
		return;
	}
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	m->index = 7;
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(bgpd_ipc_apply(m, bad[i]) == -1);
	for (size_t i = 0; i < sizeof(bad_devices) / sizeof(bad_devices[0]);
	     i++) {
		snprintf(text, sizeof(text),
			 "state\t0\t1\t1\t0\t0\t0\t0\n%send\n", bad_devices[i]);
		CHECK(bgpd_ipc_apply(m, text) == -1);
	}
	for (size_t i = 0; i < sizeof(bad_audio) / sizeof(bad_audio[0]); i++) {
		snprintf(text, sizeof(text),
			 "state\t0\t1\t1\t0\t0\t0\t0\n%send\n", bad_audio[i]);
		CHECK(bgpd_ipc_apply(m, text) == -1);
	}
	/* A bad snapshot leaves the state untouched. */
	snprintf(text, sizeof(text),
		 "state\t1\t1\t1\t0\t0\t0\t0\nstatus\tx\nbad\nend\n");
	CHECK(bgpd_ipc_apply(m, text) == -1);
	CHECK(m->index == 7 && m->status[0] == '\0');
	free(m);
}

static void test_commands(void)
{
	struct bgpd_ipc_command in, out;
	char line[64];
	size_t n;

	memset(&in, 0, sizeof(in));
	for (int kind = BGPD_IPC_SCAN; kind <= BGPD_IPC_TONE; kind++) {
		in.kind = (enum bgpd_ipc_kind)kind;
		memcpy(in.addr, remote, BGPD_ADDR_SIZE);
		n = bgpd_ipc_format(&in, line, sizeof(line));
		CHECK(n > 0 && line[n - 1] == '\n');
		line[n - 1] = '\0';
		CHECK(bgpd_ipc_parse(line, &out) == 0);
		CHECK(out.kind == in.kind);
		if (kind >= BGPD_IPC_PAIR)
			CHECK(memcmp(out.addr, remote, BGPD_ADDR_SIZE) == 0);
	}

	memset(&in, 0, sizeof(in));
	in.kind = BGPD_IPC_ACCEPT;
	snprintf(in.input, sizeof(in.input), "123456");
	n = bgpd_ipc_format(&in, line, sizeof(line));
	CHECK(strcmp(line, "accept 123456\n") == 0 && n == 14);
	line[n - 1] = '\0';
	CHECK(bgpd_ipc_parse(line, &out) == 0 && out.kind == BGPD_IPC_ACCEPT);
	CHECK(strcmp(out.input, "123456") == 0);
	CHECK(bgpd_ipc_parse("accept", &out) == 0 && out.input[0] == '\0');
	CHECK(bgpd_ipc_format(&in, line, 5) == 0);

	memset(&in, 0, sizeof(in));
	in.kind = BGPD_IPC_VOLUME;
	in.value = 42;
	n = bgpd_ipc_format(&in, line, sizeof(line));
	CHECK(strcmp(line, "volume 42\n") == 0 && n == 10);
	CHECK(bgpd_ipc_parse("volume 42", &out) == 0 && out.value == 42);
	CHECK(bgpd_ipc_parse("volume 0", &out) == 0 && out.value == 0);
	CHECK(bgpd_ipc_parse("volume 101", &out) == -1);
	CHECK(bgpd_ipc_parse("volume -1", &out) == -1);
	CHECK(bgpd_ipc_parse("volume", &out) == -1);
	CHECK(bgpd_ipc_parse("volume x", &out) == -1);

	CHECK(bgpd_ipc_parse("", &out) == -1);
	CHECK(bgpd_ipc_parse("scanning", &out) == -1);
	CHECK(bgpd_ipc_parse("scan now", &out) == -1);
	CHECK(bgpd_ipc_parse("pair", &out) == -1);
	CHECK(bgpd_ipc_parse("pair 5544", &out) == -1);
	CHECK(bgpd_ipc_parse("pair 5544332211000g", &out) == -1);
	CHECK(bgpd_ipc_parse("pair +5443322110001", &out) == -1);
	CHECK(bgpd_ipc_parse("pair 55443322110009", &out) == -1);
	CHECK(bgpd_ipc_parse("accept 12345678901234567890", &out) == -1);
	CHECK(bgpd_ipc_parse("accept 12\t3", &out) == -1);
}

int main(void)
{
	test_round_trip();
	test_malformed();
	test_commands();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
