/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "daemon.h"

#include <stdio.h>
#include <string.h>

#define TONE_MS 3000U

void bgpd_daemon_init(struct bgpd_daemon *daemon, struct bgpd_mgmt *mgmt,
		      FILE *log)
{
	memset(daemon, 0, sizeof(*daemon));
	daemon->mgmt = mgmt;
	daemon->log = log;
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		bgpd_hog_init(&daemon->slots[i].hog);
	bgpd_audio_init(&daemon->audio);
	daemon->ipc.listener = -1;
	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		daemon->ipc.peers[i].fd = -1;
}

int bgpd_daemon_listen(struct bgpd_daemon *daemon, const char *path)
{
	return bgpd_ipc_listen(&daemon->ipc, path);
}

static struct bgpd_daemon_slot *find_slot(struct bgpd_daemon *daemon,
					  const uint8_t addr[BGPD_ADDR_SIZE])
{
	struct bgpd_daemon_slot *free_slot = NULL;

	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++) {
		struct bgpd_daemon_slot *s = &daemon->slots[i];

		if (s->used && memcmp(s->addr, addr, BGPD_ADDR_SIZE) == 0)
			return s;
		if (!s->used && free_slot == NULL)
			free_slot = s;
	}
	if (free_slot != NULL) {
		free_slot->used = true;
		memcpy(free_slot->addr, addr, BGPD_ADDR_SIZE);
		free_slot->retry = 0;
	}
	return free_slot;
}

static void log_line(struct bgpd_daemon *daemon, const char *text)
{
	if (daemon->log == NULL)
		return;
	snprintf(daemon->logged, sizeof(daemon->logged), "%s", text);
	fprintf(daemon->log, "%s\n", text);
	fflush(daemon->log);
}

static void report(struct bgpd_daemon *daemon, struct bgpd_daemon_slot *slot)
{
	if (!slot->hog.changed)
		return;
	slot->hog.changed = false;
	snprintf(daemon->mgmt->status, sizeof(daemon->mgmt->status), "%s",
		 slot->hog.status);
	log_line(daemon, slot->hog.status);
}

void bgpd_daemon_log_changes(struct bgpd_daemon *daemon)
{
	if (daemon->mgmt->status[0] != '\0' &&
	    strcmp(daemon->logged, daemon->mgmt->status) != 0)
		log_line(daemon, daemon->mgmt->status);
}

static struct bgpd_daemon_slot *slot_of(struct bgpd_daemon *daemon,
					const uint8_t addr[BGPD_ADDR_SIZE])
{
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		if (daemon->slots[i].used &&
		    memcmp(daemon->slots[i].addr, addr, BGPD_ADDR_SIZE) == 0)
			return &daemon->slots[i];
	return NULL;
}

static void connect_command(struct bgpd_daemon *daemon,
			    const struct bgpd_device *d)
{
	struct bgpd_mgmt *m = daemon->mgmt;
	struct bgpd_daemon_slot *s;

	if (!d->paired) {
		snprintf(m->status, sizeof(m->status), "Pair the device first");
		return;
	}
	if (d->addr[6] == BGPD_ADDR_BREDR) {
		if (bgpd_audio_active(&daemon->audio))
			snprintf(m->status, sizeof(m->status),
				 "An audio device is already in use");
		else
			bgpd_audio_connect(&daemon->audio, d->addr, d->name);
		return;
	}
	s = find_slot(daemon, d->addr);
	if (s != NULL) {
		s->held = false;
		s->retry = 0;
	}
}

static void disconnect_command(struct bgpd_daemon *daemon,
			       const struct bgpd_device *d)
{
	struct bgpd_daemon_slot *s = slot_of(daemon, d->addr);

	if (d->addr[6] == BGPD_ADDR_BREDR &&
	    memcmp(daemon->audio.avdtp.addr, d->addr, BGPD_ADDR_SIZE) == 0)
		bgpd_audio_close(&daemon->audio);

	if (s != NULL) {
		s->held = true;
		bgpd_hog_close(&s->hog);
	}
	bgpd_mgmt_disconnect(daemon->mgmt, d);
}

static void tone_command(struct bgpd_daemon *daemon,
			 const struct bgpd_device *d)
{
	struct bgpd_mgmt *m = daemon->mgmt;
	bool same =
	    bgpd_audio_active(&daemon->audio) &&
	    memcmp(daemon->audio.avdtp.addr, d->addr, BGPD_ADDR_SIZE) == 0;

	if (d->addr[6] != BGPD_ADDR_BREDR || !d->paired) {
		snprintf(m->status, sizeof(m->status),
			 "Pair a Classic audio device first");
		return;
	}
	if (!same && bgpd_audio_active(&daemon->audio)) {
		snprintf(m->status, sizeof(m->status),
			 "An audio device is already in use");
		return;
	}
	if (!same && bgpd_audio_connect(&daemon->audio, d->addr, d->name) < 0)
		return;
	bgpd_audio_tone(&daemon->audio, TONE_MS);
}

static void unpair_command(struct bgpd_daemon *daemon,
			   const struct bgpd_device *d)
{
	struct bgpd_daemon_slot *s = slot_of(daemon, d->addr);

	if (memcmp(daemon->audio.avdtp.addr, d->addr, BGPD_ADDR_SIZE) == 0)
		bgpd_audio_close(&daemon->audio);

	/* The slot is released once the kernel reports the device unpaired. */
	if (s != NULL) {
		s->held = true;
		bgpd_hog_close(&s->hog);
	}
	bgpd_mgmt_unpair(daemon->mgmt, d);
}

static void handle(void *context, const struct bgpd_ipc_command *command)
{
	struct bgpd_daemon *daemon = context;
	struct bgpd_mgmt *m = daemon->mgmt;
	struct bgpd_device *d = NULL;

	if (command->kind >= BGPD_IPC_PAIR) {
		d = bgpd_devices_get(&m->devices, command->addr);
		if (d == NULL)
			return;
	}
	switch (command->kind) {
	case BGPD_IPC_SCAN:
		bgpd_mgmt_scan(m);
		break;
	case BGPD_IPC_POWER:
		bgpd_mgmt_toggle_power(m);
		break;
	case BGPD_IPC_ADAPTER:
		bgpd_mgmt_next_adapter(m);
		break;
	case BGPD_IPC_VISIBLE:
		bgpd_mgmt_discoverable(m);
		break;
	case BGPD_IPC_CANCEL:
		bgpd_mgmt_cancel_pair(m);
		break;
	case BGPD_IPC_ACCEPT:
		snprintf(m->input, sizeof(m->input), "%s", command->input);
		bgpd_mgmt_reply(m, true);
		break;
	case BGPD_IPC_REJECT:
		bgpd_mgmt_reply(m, false);
		break;
	case BGPD_IPC_PAIR:
		bgpd_mgmt_pair(m, d);
		break;
	case BGPD_IPC_CONNECT:
		connect_command(daemon, d);
		break;
	case BGPD_IPC_DISCONNECT:
		disconnect_command(daemon, d);
		break;
	case BGPD_IPC_UNPAIR:
		unpair_command(daemon, d);
		break;
	case BGPD_IPC_TONE:
		tone_command(daemon, d);
		break;
	}
}

/* Frees the slots of devices that are no longer paired. */
static void release_slots(struct bgpd_daemon *daemon)
{
	const struct bgpd_mgmt *m = daemon->mgmt;

	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++) {
		struct bgpd_daemon_slot *s = &daemon->slots[i];
		bool paired = false;

		if (!s->used)
			continue;
		for (size_t j = 0; j < m->devices.count; j++)
			if (memcmp(m->devices.items[j].addr, s->addr,
				   BGPD_ADDR_SIZE) == 0)
				paired = m->devices.items[j].paired;
		if (!paired) {
			bgpd_hog_close(&s->hog);
			s->used = s->held = false;
		}
	}
}

void bgpd_daemon_poll(struct bgpd_daemon *daemon)
{
	struct bgpd_mgmt *m = daemon->mgmt;
	uint64_t now = bgpd_now();

	bgpd_mgmt_poll(m);
	bgpd_ipc_serve(&daemon->ipc, m, handle, daemon);
	bgpd_audio_poll(&daemon->audio);
	if (daemon->audio.changed) {
		daemon->audio.changed = false;
		snprintf(m->status, sizeof(m->status), "%s",
			 daemon->audio.status);
		log_line(daemon, daemon->audio.status);
	}
	/* Someone must be there to answer the pairing requests of devices. */
	m->incoming = bgpd_ipc_clients(&daemon->ipc) > 0;
	if (!m->incoming && m->prompt_kind != BGPD_PROMPT_NONE)
		bgpd_mgmt_reply(m, false);
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++) {
		bgpd_hog_poll(&daemon->slots[i].hog);
		report(daemon, &daemon->slots[i]);
	}
	if (!m->ready)
		return;
	release_slots(daemon);
	for (size_t i = 0; i < m->devices.count; i++) {
		const struct bgpd_device *d = &m->devices.items[i];
		struct bgpd_daemon_slot *s;

		if (!d->paired || d->addr[6] == BGPD_ADDR_BREDR)
			continue;
		s = find_slot(daemon, d->addr);
		if (s == NULL || s->held || bgpd_hog_active(&s->hog) ||
		    now < s->retry)
			continue;
		s->retry = now + BGPD_DAEMON_RETRY_MS;
		s->hog.trace = m->trace;
		s->hog.trace_start = m->trace_start;
		bgpd_hog_connect(&s->hog, d->addr, d->name);
		report(daemon, s);
	}
}

bool bgpd_daemon_streaming(const struct bgpd_daemon *daemon)
{
	return bgpd_audio_streaming(&daemon->audio);
}

size_t bgpd_daemon_running(const struct bgpd_daemon *daemon)
{
	size_t n = 0;

	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		if (daemon->slots[i].hog.state == BGPD_HOG_RUNNING)
			n++;
	return n;
}

void bgpd_daemon_close(struct bgpd_daemon *daemon)
{
	bgpd_audio_close(&daemon->audio);
	bgpd_ipc_server_close(&daemon->ipc);
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		bgpd_hog_close(&daemon->slots[i].hog);
}
