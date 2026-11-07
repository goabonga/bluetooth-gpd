/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * ALSA PCM plugin "bluetooth": plays through bluetooth-daemon, which
 * encodes the audio to SBC and sends it to the headset or speaker in use.
 *
 *   pcm.bluetooth { type bluetooth }                  # default socket
 *   pcm.bluetooth { type bluetooth server "/path" }   # another socket
 *
 * The PCM refuses to open (ENODEV) when no audio device is connected. Its
 * sample rate is the one negotiated with the device; put a plug PCM in
 * front of it to play at any rate.
 */
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HELLO "BGPDPCM1"
#define HELLO_SIZE 8U
#define FRAME_BYTES 4U /* S16_LE, two channels */
#define ANSWER_TIMEOUT_MS 2000
#define DRAIN_TIMEOUT_MS 3000
#define SIOCOUTQ_REQUEST 0x5411UL

typedef struct {
	snd_pcm_ioplug_t io;
	int sock;
	unsigned int rate;
	uint64_t written; /* frames handed to the daemon */
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
	return (uint64_t)bytes / FRAME_BYTES;
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
	uint64_t waiting = queued(bt);
	uint64_t taken = bt->written > waiting ? bt->written - waiting : 0;

	if (io->state == SND_PCM_STATE_XRUN)
		return -EPIPE;
	return (snd_pcm_sframes_t)(taken % io->buffer_size);
}

/* Writes all of it: the daemon takes the audio at the speed of the stream. */
static snd_pcm_sframes_t bt_transfer(snd_pcm_ioplug_t *io,
				     const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset,
				     snd_pcm_uframes_t size)
{
	bluetooth *bt = io->private_data;
	const char *data = (const char *)areas->addr +
			   (areas->first + (size_t)areas->step * offset) / 8U;
	size_t left = (size_t)size * FRAME_BYTES;

	while (left > 0) {
		struct pollfd p = {.fd = bt->sock, .events = POLLOUT};
		ssize_t n;

		if (poll(&p, 1, -1) < 0 && errno != EINTR)
			return -errno;
		n = send(bt->sock, data, left, MSG_NOSIGNAL | MSG_DONTWAIT);
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

/* Connects to the daemon and returns the rate of the stream, or -errno. */
static int connect_daemon(bluetooth *bt, const char *path)
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
	if (write(bt->sock, HELLO, HELLO_SIZE) != (ssize_t)HELLO_SIZE)
		return -errno;
	p = (struct pollfd){.fd = bt->sock, .events = POLLIN};
	if (poll(&p, 1, ANSWER_TIMEOUT_MS) != 1)
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
		    &bt->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2);
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
	const char *server = NULL;
	char fallback[108];
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
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}
	if (stream != SND_PCM_STREAM_PLAYBACK)
		return -EINVAL;
	if (server == NULL) {
		const char *dir = getenv("XDG_RUNTIME_DIR");

		if (dir == NULL || dir[0] != '/' ||
		    snprintf(fallback, sizeof(fallback),
			     "%s/bluetooth-audio.sock",
			     dir) >= (int)sizeof(fallback))
			return -ENOENT;
		server = fallback;
	}
	bt = calloc(1, sizeof(*bt));
	if (bt == NULL)
		return -ENOMEM;
	bt->sock = -1;
	err = connect_daemon(bt, server);
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
	bt->io.poll_events = POLLOUT;
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
