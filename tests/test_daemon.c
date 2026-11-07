/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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

static size_t used_slots(const struct bgpd_daemon *daemon)
{
	size_t n = 0;

	for (size_t i = 0; i < BGPD_DAEMON_SLOTS; i++)
		if (daemon->slots[i].used)
			n++;
	return n;
}

static struct bgpd_device *add(struct bgpd_mgmt *m, uint8_t last, uint8_t type,
			       bool paired)
{
	const uint8_t addr[BGPD_ADDR_SIZE] = {last, 2, 3, 4, 5, 6, type};
	struct bgpd_device *d = bgpd_devices_get(&m->devices, addr);

	if (d != NULL)
		d->paired = paired;
	return d;
}

static int fake_pairs[BGPD_DAEMON_AUDIOS + 2][2];
static size_t fake_next;

/* A connected channel in place of the Bluetooth one, one for each device. */
static int fake_channel(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return fake_pairs[fake_next++][0];
}

static int no_channel(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return -1;
}

static void command(struct bgpd_ipc_client *client, struct bgpd_daemon *daemon,
		    struct bgpd_mgmt *mirror, enum bgpd_ipc_kind kind,
		    const uint8_t *addr)
{
	struct bgpd_ipc_command c = {.kind = kind};

	if (addr != NULL)
		memcpy(c.addr, addr, BGPD_ADDR_SIZE);
	CHECK(bgpd_ipc_send(client, &c) == 0);
	for (int i = 0; i < 5; i++) {
		usleep(5000);
		bgpd_daemon_poll(daemon);
		bgpd_ipc_poll(client, mirror);
	}
}

static int connect_to(const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (fd < 0 ||
	    connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		exit(2); /* the test cannot go on */
	return fd;
}

/* The rate a stream client was told: 0 when it was refused, -1 if none. */
static long told_rate(struct bgpd_daemon *daemon, int fd)
{
	uint8_t bytes[4];

	for (int i = 0; i < 40; i++) {
		bgpd_daemon_poll(daemon);
		if (recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT) == 4)
			return (long)(bytes[0] + 256U * bytes[1] +
				      65536U * bytes[2]);
		usleep(2000);
	}
	return -1;
}

static void greet(int fd, const char *magic, const uint8_t addr[BGPD_ADDR_SIZE])
{
	char text[24];

	snprintf(text, sizeof(text), "%s%02x%02x%02x%02x%02x%02x%02x\n", magic,
		 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6]);
	CHECK(write(fd, text, 23) == 23);
}

/* Several Classic audio devices are in use at once, and streams find them. */
static void test_audio_devices(struct bgpd_ipc_client *client,
			       struct bgpd_daemon *daemon,
			       struct bgpd_mgmt *mirror, struct bgpd_mgmt *m)
{
	static const uint8_t any[BGPD_ADDR_SIZE] = {0};
	struct bgpd_device *device[6];
	uint8_t address[6][BGPD_ADDR_SIZE];
	char dir[] = "/tmp/bgpd-daemon-audio-XXXXXX";
	char playback[100], capture[100];
	int stream;

	if (mkdtemp(dir) == NULL)
		return;
	snprintf(playback, sizeof(playback), "%s/audio.sock", dir);
	snprintf(capture, sizeof(capture), "%s/capture.sock", dir);
	CHECK(bgpd_daemon_listen_streams(daemon, playback, capture) == 0);
	for (size_t i = 0; i < 6; i++) {
		CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0,
				 fake_pairs[i % (BGPD_DAEMON_AUDIOS + 2)]) ==
		      0);
		device[i] = add(m, (uint8_t)(20 + i), BGPD_ADDR_BREDR, true);
		CHECK(device[i] != NULL);
		if (device[i] == NULL)
			return;
		memcpy(address[i], device[i]->addr, BGPD_ADDR_SIZE);
	}
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS; i++) {
		daemon->audios[i].audio.avdtp.open_channel = fake_channel;
		daemon->audios[i].audio.avrcp.open_channel = no_channel;
		daemon->audios[i].voice.hfp.sdp.open_channel = no_channel;
	}
	fake_next = 0;

	/* Two devices at the same time, each with its own volume. */
	command(client, daemon, mirror, BGPD_IPC_CONNECT, address[0]);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, address[1]);
	CHECK(mirror->audio_count == 2);
	CHECK(strcmp(mirror->audio[0].name, device[0]->name) == 0);
	CHECK(strcmp(mirror->audio[1].name, device[1]->name) == 0);
	{
		struct bgpd_ipc_command volume = {.kind = BGPD_IPC_VOLUME,
						  .value = 35};

		memcpy(volume.addr, address[1], BGPD_ADDR_SIZE);
		CHECK(bgpd_ipc_send(client, &volume) == 0);
		for (int i = 0; i < 5; i++) {
			usleep(5000);
			bgpd_daemon_poll(daemon);
			bgpd_ipc_poll(client, mirror);
		}
		CHECK(mirror->audio[0].volume == 100);
		CHECK(mirror->audio[1].volume == 35);
	}
	/* Their microphone goes with each of them. */
	CHECK(memcmp(daemon->audios[0].voice.target, address[0],
		     BGPD_ADDR_SIZE) == 0);
	CHECK(memcmp(daemon->audios[1].voice.target, address[1],
		     BGPD_ADDR_SIZE) == 0);
	command(client, daemon, mirror, BGPD_IPC_TONE, address[1]);
	CHECK(daemon->audios[1].audio.tone_ms > 0 &&
	      daemon->audios[0].audio.tone_ms == 0);

	/* Both are ready: streams go where their greeting says. */
	for (size_t i = 0; i < 2; i++) {
		daemon->audios[i].audio.avdtp.state = BGPD_AVDTP_READY;
		daemon->audios[i].audio.avdtp.sbc.rate = 44100;
	}
	stream = connect_to(playback);
	greet(stream, "BGPDPCM2", address[0]);
	CHECK(told_rate(daemon, stream) == 44100);
	CHECK(daemon->audios[0].audio.client >= 0 &&
	      daemon->audios[1].audio.client < 0);
	/* "Any" is the device connected last; a second client is refused. */
	{
		int second = connect_to(playback);

		greet(second, "BGPDPCM2", any);
		CHECK(told_rate(daemon, second) == 44100);
		CHECK(daemon->audios[1].audio.client >= 0);
		close(second);
	}
	{
		int third = connect_to(playback);

		greet(third, "BGPDPCM2", address[0]);
		CHECK(told_rate(daemon, third) == 0); /* already playing */
		close(third);
	}
	/* A device that is not in use is refused, and so is a microphone with
	 * no headset to reach. */
	{
		int unknown = connect_to(playback);
		int mic = connect_to(capture);

		greet(unknown, "BGPDPCM2", address[5]);
		CHECK(told_rate(daemon, unknown) == 0);
		greet(mic, "BGPDCAP2", address[0]);
		CHECK(told_rate(daemon, mic) == 0);
		close(unknown);
		close(mic);
	}
	close(stream);

	/* Leaving frees the slot; the number of devices is limited. */
	command(client, daemon, mirror, BGPD_IPC_DISCONNECT, address[0]);
	CHECK(mirror->audio_count == 1);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, address[2]);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, address[3]);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, address[4]);
	CHECK(mirror->audio_count == BGPD_DAEMON_AUDIOS);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, address[5]);
	CHECK(mirror->audio_count == BGPD_DAEMON_AUDIOS);
	CHECK(strstr(mirror->status, "Too many") != NULL);
	for (size_t i = 1; i < 5; i++)
		command(client, daemon, mirror, BGPD_IPC_DISCONNECT,
			address[i]);
	CHECK(mirror->audio_count == 0);
	CHECK(!bgpd_daemon_streaming(daemon));
	for (size_t i = 0; i < BGPD_DAEMON_AUDIOS + 2; i++) {
		close(fake_pairs[i][1]);
	}
	bgpd_streams_shutdown(&daemon->streams);
	rmdir(dir);
}

static void test_interface(void)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_mgmt *mirror = calloc(1, sizeof(*mirror));
	struct bgpd_daemon *daemon = calloc(1, sizeof(*daemon));
	struct bgpd_ipc_client *client = calloc(1, sizeof(*client));
	char dir[] = "/tmp/bgpd-daemon-XXXXXX";
	char path[128];
	struct bgpd_daemon_slot *slot;
	struct bgpd_device *d;
	uint8_t addr[BGPD_ADDR_SIZE];

	if (m == NULL || mirror == NULL || daemon == NULL || client == NULL ||
	    mkdtemp(dir) == NULL) {
		CHECK(!"memory");
		goto cleanup;
	}
	snprintf(path, sizeof(path), "%s/bluetooth.sock", dir);
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_mgmt_init(mirror, BGPD_NO_INDEX, NULL);
	bgpd_daemon_init(daemon, m, NULL);
	m->ready = true;
	d = add(m, 1, BGPD_ADDR_LE_PUBLIC, true);
	if (d == NULL) {
		CHECK(!"device");
		goto cleanup;
	}
	memcpy(addr, d->addr, BGPD_ADDR_SIZE);
	add(m, 2, BGPD_ADDR_LE_PUBLIC, false);

	CHECK(bgpd_daemon_listen(daemon, path) == 0);
	bgpd_ipc_connect(client, path);
	command(client, daemon, mirror, BGPD_IPC_CONNECT, addr);

	/* The interface sees the devices of the daemon. */
	CHECK(mirror->ready && mirror->devices.count == 2);
	CHECK(used_slots(daemon) == 1);
	slot = &daemon->slots[0];

	/* A disconnection holds the device until a connection is asked. */
	command(client, daemon, mirror, BGPD_IPC_DISCONNECT, addr);
	CHECK(slot->held && !bgpd_hog_active(&slot->hog));
	slot->retry = 0;
	bgpd_daemon_poll(daemon);
	CHECK(!bgpd_hog_active(&slot->hog));
	command(client, daemon, mirror, BGPD_IPC_CONNECT, addr);
	CHECK(!slot->held);

	/* The adapter is made visible on request. */
	m->settings = 1U;
	command(client, daemon, mirror, BGPD_IPC_VISIBLE, NULL);
	CHECK(m->prepare_action == BGPD_OP_DISCOVERABLE);

	test_audio_devices(client, daemon, mirror, m);

	/* A device that is not paired is not connected. */
	addr[0] = 2;
	command(client, daemon, mirror, BGPD_IPC_CONNECT, addr);
	CHECK(used_slots(daemon) == 1);
	CHECK(strstr(mirror->status, "Pair the device first") != NULL);

	/* Forgetting a device frees its slot once it is unpaired. */
	addr[0] = 1;
	command(client, daemon, mirror, BGPD_IPC_UNPAIR, addr);
	CHECK(slot->held);
	d->paired = false;
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 0);

	/* Pairing requests are accepted while an interface is there. */
	CHECK(m->incoming);
	bgpd_ipc_client_close(client);
	for (int i = 0; i < 5; i++) {
		usleep(5000);
		bgpd_daemon_poll(daemon);
	}
	CHECK(!m->incoming);

	/* A prompt nobody can answer is rejected. */
	{
		int fds[2];
		uint8_t packet[64];

		CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0,
				 fds) == 0);
		m->fd = fds[0];
		m->prompt_kind = BGPD_PROMPT_CONFIRM;
		bgpd_daemon_poll(daemon);
		CHECK(m->prompt_kind == BGPD_PROMPT_NONE);
		CHECK(read(fds[1], packet, sizeof(packet)) > 0);
		CHECK(bgpd_get16(packet) == BGPD_OP_CONFIRM_NEGATIVE);
		close(fds[1]);
		close(fds[0]);
		m->fd = -1;
	}
cleanup:
	if (daemon != NULL)
		bgpd_daemon_close(daemon);
	rmdir(dir);
	free(client);
	free(daemon);
	free(mirror);
	free(m);
}

int main(void)
{
	struct bgpd_mgmt *m = calloc(1, sizeof(*m));
	struct bgpd_daemon *daemon = calloc(1, sizeof(*daemon));

	if (m == NULL || daemon == NULL) {
		free(daemon);
		free(m);
		return 1;
	}
	bgpd_mgmt_init(m, BGPD_NO_INDEX, NULL);
	bgpd_daemon_init(daemon, m, NULL);

	add(m, 1, BGPD_ADDR_LE_PUBLIC, true);
	add(m, 2, BGPD_ADDR_BREDR, true);
	add(m, 3, BGPD_ADDR_LE_RANDOM, false);

	/* Nothing is connected before the adapter is known. */
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 0);

	/* Only the paired LE device gets a connection. */
	m->ready = true;
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 1);
	CHECK(daemon->slots[0].retry > 0);

	/* The same device keeps its slot and is not retried at once. */
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 1);

	/* Another paired device takes the next slot. */
	add(m, 4, BGPD_ADDR_LE_RANDOM, true);
	bgpd_daemon_poll(daemon);
	CHECK(used_slots(daemon) == 2);
	CHECK(bgpd_daemon_running(daemon) == 0);

	/* The status of the manager is logged once, and a message the daemon
	 * wrote itself is not written again. */
	{
		char *text = NULL;
		size_t size = 0;
		FILE *log = open_memstream(&text, &size);

		CHECK(log != NULL);
		daemon->log = log;
		daemon->logged[0] = '\0';
		snprintf(m->status, sizeof(m->status), "Hello");
		bgpd_daemon_log_changes(daemon);
		bgpd_daemon_log_changes(daemon);
		snprintf(m->status, sizeof(m->status), "World");
		bgpd_daemon_log_changes(daemon);
		fflush(log);
		CHECK(text != NULL && strcmp(text, "Hello\nWorld\n") == 0);
		daemon->log = NULL;
		fclose(log);
		free(text);
	}

	bgpd_daemon_close(daemon);
	free(daemon);
	free(m);
	test_interface();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
