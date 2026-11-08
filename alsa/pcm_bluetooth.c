/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * ALSA PCM plugin "bluetooth": plays through bluetooth-daemon, which
 * encodes the audio to SBC and sends it to the headset or speaker in use.
 *
 *   pcm.bluetooth { type bluetooth }                  # default sockets
 *   pcm.bluetooth { type bluetooth server "/path"     # another playback
 *                   capture_server "/path" }          # and capture socket
 *   pcm.speaker { type bluetooth device "AA:BB:CC:DD:EE:FF" }
 *
 * Without a device, the one connected last is used; with several audio
 * devices connected, "device" chooses the headset or speaker of the PCM.
 *
 * Playback sends stereo audio to the headset or speaker in use; capture
 * receives the mono microphone of the headset, which opening starts (it can
 * take several seconds). Opening fails (ENODEV) when no audio device is
 * connected. The sample rate is the one negotiated with the device (the
 * microphone is 8 or 16 kHz); put a plug PCM in front to use any rate.
 */
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HELLO_PLAYBACK "BGPDPCM2"
#define HELLO_CAPTURE "BGPDCAP2"
#define HELLO_SIZE 23U /* the kind, 14 digits for the device, a newline */
#define PLAYBACK_ANSWER_MS 2000
#define CAPTURE_ANSWER_MS 30000
#define DRAIN_TIMEOUT_MS 3000
#define SIOCOUTQ_REQUEST 0x5411UL
#define FIONREAD_REQUEST 0x541BUL

typedef struct {
	snd_pcm_ioplug_t io;
	int sock;
	unsigned int rate;
	bool capture;
	unsigned int frame_bytes; /* S16_LE: 4 for stereo, 2 for the mono mic */
	uint64_t written; /* frames handed to, or taken from, the daemon */
} bluetooth;

static void nap_ms(long ms)
{
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};

	nanosleep(&t, NULL);
}

/* Frames the daemon has not taken yet. */
static uint64_t queued(const bluetooth *bt)
{
	int bytes = 0;

	if (ioctl(bt->sock, SIOCOUTQ_REQUEST, &bytes) < 0 || bytes < 0)
		return 0;
	return (uint64_t)bytes / bt->frame_bytes;
}

/* Frames received from the daemon and not read yet. */
static uint64_t available(const bluetooth *bt)
{
	int bytes = 0;

	if (ioctl(bt->sock, FIONREAD_REQUEST, &bytes) < 0 || bytes < 0)
		return 0;
	return (uint64_t)bytes / bt->frame_bytes;
}

static int bt_start(snd_pcm_ioplug_t *io)
{
	(void)io;
	return 0;
}

static int bt_stop(snd_pcm_ioplug_t *io)
{
	(void)io;
	return 0;
}

static int bt_prepare(snd_pcm_ioplug_t *io)
{
	bluetooth *bt = io->private_data;

	bt->written = 0;
	return 0;
}

/* Position in the ring buffer of what the daemon has taken. */
static snd_pcm_sframes_t bt_pointer(snd_pcm_ioplug_t *io)
{
	bluetooth *bt = io->private_data;
	uint64_t waiting, taken;

	if (io->state == SND_PCM_STATE_XRUN)
		return -EPIPE;
	if (bt->capture) {
		uint64_t ready = available(bt);

		if (ready > io->buffer_size)
			ready = io->buffer_size;
		return (snd_pcm_sframes_t)((bt->written + ready) %
					   io->buffer_size);
	}
	waiting = queued(bt);
	taken = bt->written > waiting ? bt->written - waiting : 0;
	return (snd_pcm_sframes_t)(taken % io->buffer_size);
}

/* Writes all of it: the daemon takes the audio at the speed of the stream. */
static snd_pcm_sframes_t bt_transfer(snd_pcm_ioplug_t *io,
				     const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset,
				     snd_pcm_uframes_t size)
{
	bluetooth *bt = io->private_data;
	char *data = (char *)areas->addr +
		     (areas->first + (size_t)areas->step * offset) / 8U;
	size_t left = (size_t)size * bt->frame_bytes;

	while (left > 0) {
		struct pollfd p = {.fd = bt->sock,
				   .events = bt->capture ? POLLIN : POLLOUT};
		ssize_t n;

		if (poll(&p, 1, -1) < 0 && errno != EINTR)
			return -errno;
		if (bt->capture) {
			n = recv(bt->sock, data, left, MSG_DONTWAIT);
			if (n == 0)
				return -ENODEV; /* the daemon closed the link */
		} else {
			n = send(bt->sock, data, left,
				 MSG_NOSIGNAL | MSG_DONTWAIT);
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n < 0)
			return errno == EPIPE ? -ENODEV : -errno;
		data += n;
		left -= (size_t)n;
	}
	bt->written += size;
	return (snd_pcm_sframes_t)size;
}

static int bt_drain(snd_pcm_ioplug_t *io)
{
	bluetooth *bt = io->private_data;

	if (bt->capture)
		return 0;
	for (int waited = 0; waited < DRAIN_TIMEOUT_MS && queued(bt) > 0;
	     waited += 10)
		nap_ms(10);
	return 0;
}

static int bt_close(snd_pcm_ioplug_t *io)
{
	bluetooth *bt = io->private_data;

	if (bt->sock >= 0)
		close(bt->sock);
	free(bt);
	return 0;
}

static const snd_pcm_ioplug_callback_t callbacks = {
    .start = bt_start,
    .stop = bt_stop,
    .prepare = bt_prepare,
    .pointer = bt_pointer,
    .transfer = bt_transfer,
    .drain = bt_drain,
    .close = bt_close,
};

/*
 * The greeting for a device given as "AA:BB:CC:DD:EE:FF", or for none: the
 * kind, the address as 14 hexadecimal digits (six bytes least significant
 * first and the type 00 of a Classic device) and a newline.
 */
static int greeting(char *out, size_t size, const char *kind,
		    const char *device)
{
	unsigned int b[6] = {0};

	if (device != NULL) {
		if (strlen(device) != 17)
			return -EINVAL;
		for (size_t i = 0; i < 6; i++) {
			const char pair[3] = {device[3 * i], device[3 * i + 1],
					      '\0'};
			char *end;

			if (i < 5 && device[3 * i + 2] != ':')
				return -EINVAL;
			b[i] = (unsigned int)strtoul(pair, &end, 16);
			if (*end != '\0' || pair[0] == '+' || pair[0] == '-' ||
			    pair[0] == ' ')
				return -EINVAL;
		}
	}
	return snprintf(out, size, "%s%02x%02x%02x%02x%02x%02x00\n", kind, b[5],
			b[4], b[3], b[2], b[1], b[0]) == (int)HELLO_SIZE
		   ? 0
		   : -EINVAL;
}

/* Connects to the daemon and returns the rate of the stream, or -errno. */
static int connect_daemon(bluetooth *bt, const char *path, const char *hello,
			  int timeout_ms)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	struct pollfd p;
	uint8_t answer[4];
	ssize_t n;

	if (strlen(path) >= sizeof(address.sun_path))
		return -ENAMETOOLONG;
	memcpy(address.sun_path, path, strlen(path) + 1);
	bt->sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (bt->sock < 0)
		return -errno;
	if (connect(bt->sock, (struct sockaddr *)&address, sizeof(address)) < 0)
		return -errno;
	if (write(bt->sock, hello, HELLO_SIZE) != (ssize_t)HELLO_SIZE)
		return -errno;
	p = (struct pollfd){.fd = bt->sock, .events = POLLIN};
	if (poll(&p, 1, timeout_ms) != 1)
		return -ETIMEDOUT;
	n = read(bt->sock, answer, sizeof(answer));
	if (n != (ssize_t)sizeof(answer))
		return -ENODEV;
	bt->rate = (unsigned int)answer[0] + 256U * answer[1] +
		   65536U * answer[2] + 16777216U * answer[3];
	return bt->rate == 0 ? -ENODEV : 0;
}

static int set_constraints(bluetooth *bt)
{
	static const unsigned int access[] = {SND_PCM_ACCESS_RW_INTERLEAVED,
					      SND_PCM_ACCESS_MMAP_INTERLEAVED};
	static const unsigned int format[] = {SND_PCM_FORMAT_S16_LE};
	int err;

	err = snd_pcm_ioplug_set_param_list(&bt->io, SND_PCM_IOPLUG_HW_ACCESS,
					    2, access);
	if (err >= 0)
		err = snd_pcm_ioplug_set_param_list(
		    &bt->io, SND_PCM_IOPLUG_HW_FORMAT, 1, format);
	if (err >= 0)
		err = snd_pcm_ioplug_set_param_minmax(
		    &bt->io, SND_PCM_IOPLUG_HW_CHANNELS, bt->capture ? 1 : 2,
		    bt->capture ? 1 : 2);
	if (err >= 0)
		err = snd_pcm_ioplug_set_param_minmax(
		    &bt->io, SND_PCM_IOPLUG_HW_RATE, bt->rate, bt->rate);
	if (err >= 0)
		err = snd_pcm_ioplug_set_param_minmax(
		    &bt->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1024, 65536);
	if (err >= 0)
		err = snd_pcm_ioplug_set_param_minmax(
		    &bt->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64);
	return err;
}

/* The ALSA macros fix the signature, whose root parameter is unused, and
 * end the plugin symbol with a semicolon of their own. */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wextra-semi"
#endif
SND_PCM_PLUGIN_DEFINE_FUNC(bluetooth)
{
	snd_config_iterator_t i, next;
	const char *server = NULL, *capture_server = NULL, *device = NULL;
	char fallback[108], hello[HELLO_SIZE + 1];
	bluetooth *bt;
	int err;

	snd_config_for_each(i, next, conf)
	{
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;

		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 ||
		    strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "server") == 0) {
			if (snd_config_get_string(n, &server) < 0) {
				SNDERR("server must be a string");
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "device") == 0) {
			if (snd_config_get_string(n, &device) < 0) {
				SNDERR("device must be a string");
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "capture_server") == 0) {
			if (snd_config_get_string(n, &capture_server) < 0) {
				SNDERR("capture_server must be a string");
				return -EINVAL;
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}
	if (stream == SND_PCM_STREAM_CAPTURE)
		server = capture_server;
	if (server == NULL) {
		const char *dir = getenv("BLUETOOTH_RUNTIME_DIR");

		if (dir == NULL || dir[0] != '/')
			dir = "/run/bluetooth";
		if (snprintf(fallback, sizeof(fallback), "%s/%s", dir,
			     stream == SND_PCM_STREAM_CAPTURE
				 ? "bluetooth-capture.sock"
				 : "bluetooth-audio.sock") >=
		    (int)sizeof(fallback))
			return -ENOENT;
		server = fallback;
	}
	bt = calloc(1, sizeof(*bt));
	if (bt == NULL)
		return -ENOMEM;
	bt->sock = -1;
	bt->capture = stream == SND_PCM_STREAM_CAPTURE;
	bt->frame_bytes = bt->capture ? 2U : 4U;
	err = greeting(hello, sizeof(hello),
		       bt->capture ? HELLO_CAPTURE : HELLO_PLAYBACK, device);
	if (err < 0) {
		SNDERR("device must be an address like AA:BB:CC:DD:EE:FF");
		free(bt);
		return err;
	}
	err = connect_daemon(bt, server, hello,
			     bt->capture ? CAPTURE_ANSWER_MS
					 : PLAYBACK_ANSWER_MS);
	if (err < 0) {
		SNDERR("No Bluetooth audio device ready (%s): %s", server,
		       strerror(-err));
		if (bt->sock >= 0)
			close(bt->sock);
		free(bt);
		return err;
	}
	bt->io.version = SND_PCM_IOPLUG_VERSION;
	bt->io.name = "Bluetooth audio";
	bt->io.callback = &callbacks;
	bt->io.private_data = bt;
	bt->io.poll_fd = bt->sock;
	bt->io.poll_events = bt->capture ? POLLIN : POLLOUT;
	err = snd_pcm_ioplug_create(&bt->io, name, stream, mode);
	if (err < 0) {
		close(bt->sock);
		free(bt);
		return err;
	}
	err = set_constraints(bt);
	if (err < 0) {
		snd_pcm_ioplug_delete(&bt->io);
		return err;
	}
	*pcmp = bt->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(bluetooth)
#ifdef __clang__
#pragma clang diagnostic pop
#endif
