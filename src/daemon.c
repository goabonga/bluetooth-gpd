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
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++) {
		bgpd_audio_init(&daemon->audios[i].audio);
		bgpd_voice_init(&daemon->audios[i].voice);
	}
	bgpd_streams_init(&daemon->streams);
	daemon->ipc.listener = -1;
	for (size_t i = 0; i < BGPD_IPC_MAX_CLIENTS; i++)
		daemon->ipc.peers[i].fd = -1;
}

int bgpd_daemon_listen(struct bgpd_daemon *daemon, const char *path)
{
	return bgpd_ipc_listen(&daemon->ipc, path);
}

int bgpd_daemon_listen_streams(struct bgpd_daemon *daemon,
			       const char *audio_path, const char *capture_path)
{
	if (bgpd_streams_listen(&daemon->streams, BGPD_STREAM_PLAYBACK,
				audio_path) < 0)
		return -1;
	return bgpd_streams_listen(&daemon->streams, BGPD_STREAM_CAPTURE,
				   capture_path);
}

/* ---- the audio devices ---- */

static struct bgpd_daemon_audio *find_audio(struct bgpd_daemon *daemon,
					    const uint8_t addr[BGPD_ADDR_SIZE])
{
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++) {
		struct bgpd_daemon_audio *a = &daemon->audios[i];

		if (a->used &&
		    memcmp(a->audio.avdtp.addr, addr, BGPD_ADDR_SIZE) == 0)
			return a;
	}
	return NULL;
}

/* Connects an audio device, in a free slot; NULL if it cannot. */
static struct bgpd_daemon_audio *open_audio(struct bgpd_daemon *daemon,
					    const struct bgpd_device *d)
{
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++) {
		struct bgpd_daemon_audio *a = &daemon->audios[i];

		if (a->used)
			continue;
		if (bgpd_audio_connect(&a->audio, d->addr, d->name) < 0)
			return NULL;
		a->used = true;
		bgpd_voice_target(&a->voice, d->addr, d->name);
		daemon->last_audio = i;
		return a;
	}
	snprintf(daemon->mgmt->status, sizeof(daemon->mgmt->status),
		 "Too many audio devices");
	return NULL;
}

static void close_audio(struct bgpd_daemon_audio *a)
{
	bgpd_voice_shutdown(&a->voice);
	bgpd_audio_close(&a->audio);
	a->used = false;
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
		if (find_audio(daemon, d->addr) != NULL)
			snprintf(m->status, sizeof(m->status),
				 "%s is already connected", d->name);
		else
			open_audio(daemon, d);
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

	if (d->addr[6] == BGPD_ADDR_BREDR) {
		struct bgpd_daemon_audio *a = find_audio(daemon, d->addr);

		if (a != NULL)
			close_audio(a);
	}

	if (s != NULL) {
		s->held = true;
		bgpd_hog_close(&s->hog);
	}
	bgpd_mgmt_disconnect(daemon->mgmt, d);
}

static void tone_command(struct bgpd_daemon *daemon,
			 const struct bgpd_device *d)
{
	struct bgpd_daemon_audio *a;

	if (d->addr[6] != BGPD_ADDR_BREDR || !d->paired) {
		snprintf(daemon->mgmt->status, sizeof(daemon->mgmt->status),
			 "Pair a Classic audio device first");
		return;
	}
	a = find_audio(daemon, d->addr);
	if (a == NULL)
		a = open_audio(daemon, d);
	if (a != NULL)
		bgpd_audio_tone(&a->audio, TONE_MS);
}

static void unpair_command(struct bgpd_daemon *daemon,
			   const struct bgpd_device *d)
{
	struct bgpd_daemon_slot *s = slot_of(daemon, d->addr);

	if (d->addr[6] == BGPD_ADDR_BREDR) {
		struct bgpd_daemon_audio *a = find_audio(daemon, d->addr);

		if (a != NULL)
			close_audio(a);
	}

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

	if (command->kind >= BGPD_IPC_PAIR && command->kind <= BGPD_IPC_TONE) {
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
	case BGPD_IPC_VOLUME: {
		struct bgpd_daemon_audio *a = find_audio(daemon, command->addr);

		if (a != NULL)
			bgpd_audio_set_volume(&a->audio,
					      (unsigned int)command->value);
		break;
	}
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

/* Copies the audio device in use where the interfaces get their state. */
static void publish_audio(struct bgpd_daemon *daemon)
{
	struct bgpd_mgmt *m = daemon->mgmt;

	m->audio_count = 0;
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++) {
		const struct bgpd_audio *audio = &daemon->audios[i].audio;
		struct bgpd_audio_info *info;

		if (!daemon->audios[i].used)
			continue;
		info = &m->audio[m->audio_count++];
		info->active = true;
		memcpy(info->addr, audio->avdtp.addr, BGPD_ADDR_SIZE);
		snprintf(info->name, sizeof(info->name), "%s",
			 audio->avdtp.name);
		info->volume = (int)audio->volume;
		if (audio->avdtp.state == BGPD_AVDTP_STREAMING)
			info->state = BGPD_AUDIO_STREAMING;
		else if (audio->avdtp.state >= BGPD_AVDTP_READY)
			info->state = BGPD_AUDIO_READY;
		else
			info->state = BGPD_AUDIO_CONNECTING;
	}
}

/* The device a client asked for; "any" is the one connected last. */
static struct bgpd_daemon_audio *pick_audio(struct bgpd_daemon *daemon,
					    const struct bgpd_stream_request *r)
{
	struct bgpd_daemon_audio *last = &daemon->audios[daemon->last_audio];

	if (!r->any)
		return find_audio(daemon, r->addr);
	if (last->used)
		return last;
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++)
		if (daemon->audios[i].used)
			return &daemon->audios[i];
	return NULL;
}

/* Gives each client of the PCM sockets the device its greeting names. */
static void route_streams(struct bgpd_daemon *daemon)
{
	struct bgpd_stream_request request;

	while (bgpd_streams_next(&daemon->streams, &request)) {
		struct bgpd_daemon_audio *a = pick_audio(daemon, &request);

		if (a == NULL)
			bgpd_stream_refuse(request.fd);
		else if (request.kind == BGPD_STREAM_PLAYBACK)
			bgpd_audio_attach(&a->audio, request.fd);
		else
			bgpd_voice_attach(&a->voice, request.fd);
	}
}

void bgpd_daemon_poll(struct bgpd_daemon *daemon)
{
	struct bgpd_mgmt *m = daemon->mgmt;
	uint64_t now = bgpd_now();

	bgpd_mgmt_poll(m);
	bgpd_ipc_serve(&daemon->ipc, m, handle, daemon);
	route_streams(daemon);
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++) {
		struct bgpd_daemon_audio *a = &daemon->audios[i];

		if (!a->used)
			continue;
		bgpd_audio_poll(&a->audio);
		bgpd_voice_poll(&a->voice);
		if (a->audio.changed) {
			a->audio.changed = false;
			snprintf(m->status, sizeof(m->status), "%s",
				 a->audio.status);
			log_line(daemon, a->audio.status);
		}
		/* A device that could not connect, or left, frees its slot. */
		if (!bgpd_audio_active(&a->audio))
			close_audio(a);
	}
	publish_audio(daemon);
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
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++)
		if (bgpd_audio_streaming(&daemon->audios[i].audio) ||
		    bgpd_voice_streaming(&daemon->audios[i].voice))
			return true;
	return false;
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
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++)
		close_audio(&daemon->audios[i]);
	bgpd_streams_shutdown(&daemon->streams);
	bgpd_ipc_server_close(&daemon->ipc);
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		bgpd_hog_close(&daemon->slots[i].hog);
}
