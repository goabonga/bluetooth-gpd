/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "ipc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 8

#define FLAG_PAIRED 1U
#define FLAG_SAVED 2U
#define FLAG_CONNECTED 4U
#define FLAG_RSSI 8U
#define HEX_LENGTH 14U /* two digits for each of the seven address bytes */

static const struct {
	const char *word;
	enum bgpd_ipc_kind kind;
	bool address, input, number;
} commands[] = {
    {"scan", BGPD_IPC_SCAN, false, false, false},
    {"power", BGPD_IPC_POWER, false, false, false},
    {"adapter", BGPD_IPC_ADAPTER, false, false, false},
    {"visible", BGPD_IPC_VISIBLE, false, false, false},
    {"cancel", BGPD_IPC_CANCEL, false, false, false},
    {"accept", BGPD_IPC_ACCEPT, false, true, false},
    {"reject", BGPD_IPC_REJECT, false, false, false},
    {"pair", BGPD_IPC_PAIR, true, false, false},
    {"connect", BGPD_IPC_CONNECT, true, false, false},
    {"disconnect", BGPD_IPC_DISCONNECT, true, false, false},
    {"unpair", BGPD_IPC_UNPAIR, true, false, false},
    {"tone", BGPD_IPC_TONE, true, false, false},
    {"volume", BGPD_IPC_VOLUME, false, false, true},
};

/* ---- writing ---- */

struct out {
	char *text;
	size_t size, used;
	bool full;
};

static void put(struct out *o, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void put(struct out *o, const char *format, ...)
{
	va_list args;
	int n;

	if (o->full)
		return;
	va_start(args, format);
	n = vsnprintf(o->text + o->used, o->size - o->used, format, args);
	va_end(args);
	if (n < 0 || (size_t)n >= o->size - o->used)
		o->full = true;
	else
		o->used += (size_t)n;
}

/* Free text goes in one field: no tab, newline or control character. */
static void clean(const char *text, char *out, size_t size)
{
	size_t i = 0;

	for (; text[i] != '\0' && i + 1 < size; i++)
		out[i] = (unsigned char)text[i] < ' ' ? ' ' : text[i];
	out[i] = '\0';
}

static void hex(const uint8_t addr[BGPD_ADDR_SIZE], char out[HEX_LENGTH + 1])
{
	for (size_t i = 0; i < BGPD_ADDR_SIZE; i++)
		snprintf(out + 2 * i, 3, "%02x", addr[i]);
}

static bool unhex(const char *text, uint8_t addr[BGPD_ADDR_SIZE])
{
	if (strlen(text) != HEX_LENGTH)
		return false;
	for (size_t i = 0; i < BGPD_ADDR_SIZE; i++) {
		char pair[3] = {text[2 * i], text[2 * i + 1], '\0'};
		char *end;
		unsigned long n;

		if (pair[0] == '-' || pair[0] == '+' || pair[0] == ' ')
			return false;
		n = strtoul(pair, &end, 16);
		if (*end != '\0')
			return false;
		addr[i] = (uint8_t)n;
	}
	return addr[6] <= BGPD_ADDR_LE_RANDOM;
}

size_t bgpd_ipc_snapshot(const struct bgpd_mgmt *m, char *out, size_t size)
{
	struct out o = {.text = out, .size = size};
	char text[BGPD_STATUS_SIZE];

	put(&o, "state\t%u\t%d\t%u\t%d\t%d\t%d\t%u\n", (unsigned int)m->index,
	    m->ready, (unsigned int)m->settings, m->scanning, m->pairing,
	    m->command != 0, (unsigned int)m->prompt_kind);
	clean(m->controller_name, text, sizeof(text));
	put(&o, "adapter\t%s\t%s\n", m->controller_address, text);
	clean(m->status, text, sizeof(text));
	put(&o, "status\t%s\n", text);
	clean(m->warning, text, sizeof(text));
	put(&o, "warning\t%s\n", text);
	clean(m->prompt, text, sizeof(text));
	put(&o, "prompt\t%s\n", text);
	for (size_t i = 0; i < m->devices.count; i++) {
		const struct bgpd_device *d = &m->devices.items[i];
		char address[HEX_LENGTH + 1], name[BGPD_NAME_SIZE];
		unsigned int flags = (d->paired ? FLAG_PAIRED : 0U) |
				     (d->saved ? FLAG_SAVED : 0U) |
				     (d->connected ? FLAG_CONNECTED : 0U) |
				     (d->has_rssi ? FLAG_RSSI : 0U);

		hex(d->addr, address);
		clean(d->name, name, sizeof(name));
		put(&o, "device\t%s\t%u\t%d\t%s\n", address, flags, d->rssi,
		    name);
	}
	if (m->audio.active) {
		static const char *const states[] = {"connecting", "ready",
						     "streaming"};
		char address[HEX_LENGTH + 1], name[BGPD_NAME_SIZE];

		hex(m->audio.addr, address);
		clean(m->audio.name, name, sizeof(name));
		put(&o, "audio\t%s\t%s\t%d\t%s\n", address,
		    states[m->audio.state], m->audio.volume, name);
	}
	put(&o, "end\n");
	return o.full ? 0 : o.used;
}

/* ---- reading ---- */

/* Splits a line at its tabs, in place. The last field keeps the rest. */
static size_t split(char *line, char *fields[], size_t max)
{
	size_t n = 0;

	fields[n++] = line;
	while (n < max && (line = strchr(line, '\t')) != NULL) {
		*line++ = '\0';
		fields[n++] = line;
	}
	return n;
}

static bool number(const char *text, long min, long max, long *value)
{
	char *end;

	errno = 0;
	*value = strtol(text, &end, 10);
	return text[0] != '\0' && *end == '\0' && errno == 0 && *value >= min &&
	       *value <= max;
}

/*
 * Reads a snapshot; only writes to m when apply is set, so that a first
 * pass can check the whole text.
 */
static int read_snapshot(struct bgpd_mgmt *m, const char *snapshot, bool apply)
{
	char line[BGPD_NAME_SIZE + 128];
	size_t count = 0;
	bool state = false, ended = false, audio = false;

	if (apply)
		m->devices.count = 0;
	while (*snapshot != '\0' && !ended) {
		const char *eol = strchr(snapshot, '\n');
		size_t length;
		char *f[MAX_FIELDS];
		size_t n;
		long v[7];

		if (eol == NULL)
			return -1;
		length = (size_t)(eol - snapshot);
		if (length >= sizeof(line))
			return -1;
		memcpy(line, snapshot, length);
		line[length] = '\0';
		snapshot = eol + 1;
		n = split(line, f, MAX_FIELDS);

		if (strcmp(f[0], "state") == 0 && n == 8) {
			if (!number(f[1], 0, 0xffff, &v[0]) ||
			    !number(f[2], 0, 1, &v[1]) ||
			    !number(f[3], 0, 0x7fffffff, &v[2]) ||
			    !number(f[4], 0, 1, &v[3]) ||
			    !number(f[5], 0, 1, &v[4]) ||
			    !number(f[6], 0, 1, &v[5]) ||
			    !number(f[7], 0, 0xffff, &v[6]))
				return -1;
			state = true;
			if (apply) {
				m->index = (uint16_t)v[0];
				m->ready = v[1] != 0;
				m->settings = (uint32_t)v[2];
				m->scanning = v[3] != 0;
				m->pairing = v[4] != 0;
				m->command = v[5] != 0 ? 1U : 0U;
				m->prompt_kind = (uint16_t)v[6];
			}
		} else if (strcmp(f[0], "adapter") == 0 && n == 3) {
			if (strlen(f[1]) >= BGPD_ADDR_TEXT)
				return -1;
			if (apply) {
				snprintf(m->controller_address,
					 sizeof(m->controller_address), "%s",
					 f[1]);
				snprintf(m->controller_name,
					 sizeof(m->controller_name), "%s",
					 f[2]);
			}
		} else if (strcmp(f[0], "status") == 0 && n == 2) {
			if (apply)
				snprintf(m->status, sizeof(m->status), "%s",
					 f[1]);
		} else if (strcmp(f[0], "warning") == 0 && n == 2) {
			if (apply)
				snprintf(m->warning, sizeof(m->warning), "%s",
					 f[1]);
		} else if (strcmp(f[0], "prompt") == 0 && n == 2) {
			if (apply)
				snprintf(m->prompt, sizeof(m->prompt), "%s",
					 f[1]);
		} else if (strcmp(f[0], "device") == 0 && n == 5) {
			uint8_t addr[BGPD_ADDR_SIZE];

			if (!unhex(f[1], addr) || !number(f[2], 0, 15, &v[0]) ||
			    !number(f[3], -128, 127, &v[1]) ||
			    ++count > BGPD_MAX_DEVICES)
				return -1;
			if (apply) {
				struct bgpd_device *d =
				    bgpd_devices_get(&m->devices, addr);

				if (d == NULL)
					return -1;
				snprintf(d->name, sizeof(d->name), "%s", f[4]);
				unsigned int flags = (unsigned int)v[0];

				d->paired = (flags & FLAG_PAIRED) != 0;
				d->saved = (flags & FLAG_SAVED) != 0;
				d->connected = (flags & FLAG_CONNECTED) != 0;
				d->has_rssi = (flags & FLAG_RSSI) != 0;
				d->rssi = (int)v[1];
			}
		} else if (strcmp(f[0], "audio") == 0 && n == 5) {
			static const char *const states[] = {
			    "connecting", "ready", "streaming"};
			uint8_t addr[BGPD_ADDR_SIZE];
			size_t state = 0;

			while (state < 3 && strcmp(f[2], states[state]) != 0)
				state++;
			if (!unhex(f[1], addr) || state == 3 ||
			    !number(f[3], -1, 100, &v[0]))
				return -1;
			audio = true;
			if (apply) {
				m->audio.active = true;
				memcpy(m->audio.addr, addr, BGPD_ADDR_SIZE);
				m->audio.state = (enum bgpd_audio_state)state;
				m->audio.volume = (int)v[0];
				snprintf(m->audio.name, sizeof(m->audio.name),
					 "%s", f[4]);
			}
		} else if (strcmp(f[0], "end") == 0 && n == 1) {
			ended = true;
		} else {
			return -1;
		}
	}
	if (apply && !audio)
		m->audio.active = false;
	return state && ended ? 0 : -1;
}

int bgpd_ipc_apply(struct bgpd_mgmt *m, const char *snapshot)
{
	if (read_snapshot(m, snapshot, false) < 0)
		return -1;
	return read_snapshot(m, snapshot, true);
}

size_t bgpd_ipc_format(const struct bgpd_ipc_command *command, char *out,
		       size_t size)
{
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		char address[HEX_LENGTH + 1];
		int n;

		if (commands[i].kind != command->kind)
			continue;
		if (commands[i].address) {
			hex(command->addr, address);
			n = snprintf(out, size, "%s %s\n", commands[i].word,
				     address);
		} else if (commands[i].number) {
			n = snprintf(out, size, "%s %d\n", commands[i].word,
				     command->value);
		} else if (commands[i].input && command->input[0] != '\0') {
			n = snprintf(out, size, "%s %s\n", commands[i].word,
				     command->input);
		} else {
			n = snprintf(out, size, "%s\n", commands[i].word);
		}
		return n > 0 && (size_t)n < size ? (size_t)n : 0;
	}
	return 0;
}

int bgpd_ipc_parse(const char *line, struct bgpd_ipc_command *command)
{
	const char *argument = strchr(line, ' ');
	size_t word =
	    argument != NULL ? (size_t)(argument - line) : strlen(line);

	memset(command, 0, sizeof(*command));
	if (argument != NULL)
		argument++;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (strlen(commands[i].word) != word ||
		    strncmp(line, commands[i].word, word) != 0)
			continue;
		command->kind = commands[i].kind;
		if (commands[i].address)
			return argument != NULL &&
				       unhex(argument, command->addr)
				   ? 0
				   : -1;
		if (commands[i].number) {
			long value;

			if (argument == NULL ||
			    !number(argument, 0, 100, &value))
				return -1;
			command->value = (int)value;
			return 0;
		}
		if (commands[i].input && argument != NULL) {
			if (strlen(argument) >= sizeof(command->input))
				return -1;
			for (const char *c = argument; *c != '\0'; c++)
				if ((unsigned char)*c < ' ' || *c > '~')
					return -1;
			memcpy(command->input, argument, strlen(argument) + 1);
			return 0;
		}
		return argument == NULL ? 0 : -1;
	}
	return -1;
}
