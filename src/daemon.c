/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "daemon.h"

#include <string.h>

void bgpd_daemon_init(struct bgpd_daemon *daemon, struct bgpd_mgmt *mgmt,
		      FILE *log)
{
	memset(daemon, 0, sizeof(*daemon));
	daemon->mgmt = mgmt;
	daemon->log = log;
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		bgpd_hog_init(&daemon->slots[i].hog);
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

static void report(struct bgpd_daemon *daemon, struct bgpd_daemon_slot *slot)
{
	if (!slot->hog.changed)
		return;
	slot->hog.changed = false;
	if (daemon->log != NULL) {
		fprintf(daemon->log, "%s\n", slot->hog.status);
		fflush(daemon->log);
	}
}

void bgpd_daemon_poll(struct bgpd_daemon *daemon)
{
	struct bgpd_mgmt *m = daemon->mgmt;
	uint64_t now = bgpd_now();

	bgpd_mgmt_poll(m);
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++) {
		bgpd_hog_poll(&daemon->slots[i].hog);
		report(daemon, &daemon->slots[i]);
	}
	if (!m->ready)
		return;
	for (size_t i = 0; i < m->devices.count; i++) {
		const struct bgpd_device *d = &m->devices.items[i];
		struct bgpd_daemon_slot *s;

		if (!d->paired || d->addr[6] == BGPD_ADDR_BREDR)
			continue;
		s = find_slot(daemon, d->addr);
		if (s == NULL || bgpd_hog_active(&s->hog) || now < s->retry)
			continue;
		s->retry = now + BGPD_DAEMON_RETRY_MS;
		s->hog.trace = m->trace;
		s->hog.trace_start = m->trace_start;
		bgpd_hog_connect(&s->hog, d->addr, d->name);
		report(daemon, s);
	}
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
	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		bgpd_hog_close(&daemon->slots[i].hog);
}
