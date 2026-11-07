/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "audio.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TONE_HZ 440.0
#define TONE_AMPLITUDE 6000.0
#define PI 3.14159265358979323846
/* Audio is sent this far ahead of real time, to absorb the polling delay. */
#define LEAD_MS 60U
#define MAX_FRAMES_PER_PACKET 15U
#define CHANNELS 2U

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

void bgpd_audio_init(struct bgpd_audio *audio)
{
	memset(audio, 0, sizeof(*audio));
	audio->listener = audio->client = -1;
	bgpd_avdtp_init(&audio->avdtp);
}

bool bgpd_audio_active(const struct bgpd_audio *audio)
{
	return bgpd_avdtp_active(&audio->avdtp);
}

bool bgpd_audio_streaming(const struct bgpd_audio *audio)
{
	return audio->avdtp.state == BGPD_AVDTP_STREAMING;
}

int bgpd_audio_default_path(char *buffer, size_t size)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int length;

	if (dir == NULL || dir[0] != '/') {
		errno = ENOENT;
		return -1;
	}
	length = snprintf(buffer, size, "%s/bluetooth-audio.sock", dir);
	if (length < 0 || (size_t)length >= size ||
	    (size_t)length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

int bgpd_audio_listen(struct bgpd_audio *audio, const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int fd, saved;

	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(address.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int probe;
		bool alive;

		if (errno != EADDRINUSE)
			goto fail;
		/* A socket left by a stopped daemon is replaced. */
		probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		alive =
		    probe >= 0 && connect(probe, (struct sockaddr *)&address,
					  sizeof(address)) == 0;
		if (probe >= 0)
			close(probe);
		if (alive) {
			errno = EADDRINUSE;
			goto fail;
		}
		unlink(path);
		if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
			goto fail;
	}
	if (chmod(path, S_IRUSR | S_IWUSR) < 0 || listen(fd, 2) < 0) {
		saved = errno;
		unlink(path);
		errno = saved;
		goto fail;
	}
	audio->listener = fd;
	snprintf(audio->path, sizeof(audio->path), "%s", path);
	return 0;
fail:
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}

static void drop_client(struct bgpd_audio *audio)
{
	if (audio->client >= 0)
		close(audio->client);
	audio->client = -1;
	audio->hello_used = 0;
}

/* Takes the new clients: only one at a time, and only when a sink is ready. */
static void accept_client(struct bgpd_audio *audio)
{
	for (;;) {
		struct ucred cred;
		socklen_t length = sizeof(cred);
		int fd = accept4(audio->listener, NULL, NULL,
				 SOCK_NONBLOCK | SOCK_CLOEXEC);
		bool ready;

		if (fd < 0 && errno == EINTR)
			continue;
		if (fd < 0)
			return;
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) <
			0 ||
		    cred.uid != geteuid()) {
			close(fd);
			continue;
		}
		ready = audio->client < 0 &&
			audio->avdtp.state >= BGPD_AVDTP_READY &&
			audio->avdtp.sbc.rate != 0;
		if (ready) {
			audio->client = fd;
			audio->hello_used = 0;
		} else {
			/* The rate 0 refuses the stream. */
			static const uint8_t refused[4] = {0};
			ssize_t n = send(fd, refused, sizeof(refused),
					 MSG_NOSIGNAL | MSG_DONTWAIT);

			(void)n;
			close(fd);
		}
	}
}

/* Reads the greeting, answers it, then takes the PCM into the ring. */
static void read_client(struct bgpd_audio *audio)
{
	while (audio->client >= 0) {
		ssize_t n;

		if (audio->hello_used < BGPD_AUDIO_HELLO_SIZE) {
			n = read(audio->client,
				 audio->hello + audio->hello_used,
				 BGPD_AUDIO_HELLO_SIZE - audio->hello_used);
		} else {
			size_t tail = (audio->ring_head + audio->ring_used) %
				      BGPD_AUDIO_RING;
			size_t room = BGPD_AUDIO_RING - audio->ring_used;

			if (room == 0)
				return; /* the client waits */
			if (room > BGPD_AUDIO_RING - tail)
				room = BGPD_AUDIO_RING - tail;
			n = read(audio->client, audio->ring + tail, room);
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			drop_client(audio);
			return;
		}
		if (audio->hello_used < BGPD_AUDIO_HELLO_SIZE) {
			audio->hello_used += (size_t)n;
			if (audio->hello_used < BGPD_AUDIO_HELLO_SIZE)
				continue;
			if (memcmp(audio->hello, BGPD_AUDIO_HELLO,
				   BGPD_AUDIO_HELLO_SIZE) != 0) {
				drop_client(audio);
				return;
			}
			{
				uint8_t rate[4];
				unsigned int value = audio->avdtp.sbc.rate;

				for (unsigned int i = 0; i < 4; i++)
					rate[i] = (uint8_t)(value >> (8U * i));
				if (send(audio->client, rate, sizeof(rate),
					 MSG_NOSIGNAL | MSG_DONTWAIT) !=
				    (ssize_t)sizeof(rate)) {
					drop_client(audio);
					return;
				}
			}
		} else {
			audio->ring_used += (size_t)n;
		}
	}
}

static void finish_encoder(struct bgpd_audio *audio)
{
	if (audio->encoding)
		sbc_finish(&audio->sbc);
	audio->encoding = false;
}

void bgpd_audio_close(struct bgpd_audio *audio)
{
	finish_encoder(audio);
	bgpd_avdtp_close(&audio->avdtp);
	audio->tone_ms = audio->tone_left = 0;
	drop_client(audio);
	audio->ring_head = audio->ring_used = 0;
}

void bgpd_audio_shutdown(struct bgpd_audio *audio)
{
	bgpd_audio_close(audio);
	if (audio->listener >= 0) {
		close(audio->listener);
		unlink(audio->path);
	}
	audio->listener = -1;
}

int bgpd_audio_connect(struct bgpd_audio *audio,
		       const uint8_t addr[BGPD_ADDR_SIZE], const char *name)
{
	return bgpd_avdtp_connect(&audio->avdtp, addr, name);
}

int bgpd_audio_tone(struct bgpd_audio *audio, unsigned int milliseconds)
{
	if (!bgpd_avdtp_active(&audio->avdtp))
		return -1;
	/* The rate is known once the sink is configured. */
	audio->tone_ms = milliseconds;
	audio->tone_left = 0;
	audio->tone_phase = 0;
	return 0;
}

/* Sets the encoder up from the codec information the sink was given. */
static int start_encoder(struct bgpd_audio *audio)
{
	if (sbc_init_a2dp(&audio->sbc, 0, audio->avdtp.sbc.info,
			  sizeof(audio->avdtp.sbc.info)) < 0)
		return -1;
	audio->sbc.bitpool = audio->avdtp.sbc.bitpool;
	audio->sbc.endian = SBC_LE;
	audio->encoding = true;
	audio->frame_pcm = sbc_get_codesize(&audio->sbc);
	audio->frame_size = sbc_get_frame_length(&audio->sbc);
	audio->frame_samples =
	    (unsigned int)(audio->frame_pcm / (CHANNELS * sizeof(int16_t)));
	audio->epoch = now_ms();
	audio->frames_sent = 0;
	audio->sent_any = false;
	audio->tone_left = (uint32_t)((uint64_t)audio->tone_ms *
				      audio->avdtp.sbc.rate / 1000U);
	audio->tone_ms = 0;
	return audio->frame_pcm > 0 && audio->frame_size > 0 &&
		       audio->frame_samples > 0
		   ? 0
		   : -1;
}

/* One frame of the test tone, as interleaved stereo samples. */
static void tone_frame(struct bgpd_audio *audio, int16_t *pcm)
{
	unsigned int rate = audio->avdtp.sbc.rate;

	for (unsigned int i = 0; i < audio->frame_samples; i++) {
		double angle = 2.0 * PI * TONE_HZ * audio->tone_phase / rate;
		int16_t sample = (int16_t)(TONE_AMPLITUDE * sin(angle));

		pcm[(size_t)i * CHANNELS] = sample;
		pcm[(size_t)i * CHANNELS + 1] = sample;
		audio->tone_phase++;
	}
	audio->tone_left = audio->tone_left > audio->frame_samples
			       ? audio->tone_left - audio->frame_samples
			       : 0;
}

/* True when a frame of audio can be taken from the tone or the client. */
static bool have_frame(const struct bgpd_audio *audio)
{
	return audio->tone_left > 0 || audio->ring_used >= audio->frame_pcm;
}

/* Takes the next frame of PCM: the tone first, else what the client sent. */
static void next_frame(struct bgpd_audio *audio, int16_t *pcm)
{
	uint8_t *bytes = (uint8_t *)pcm;

	if (audio->tone_left > 0) {
		tone_frame(audio, pcm);
		return;
	}
	for (size_t i = 0; i < audio->frame_pcm; i++) {
		bytes[i] = audio->ring[audio->ring_head];
		audio->ring_head = (audio->ring_head + 1U) % BGPD_AUDIO_RING;
	}
	audio->ring_used -= audio->frame_pcm;
}

/* Encodes and sends the frames the sink needs by now. */
static void stream(struct bgpd_audio *audio)
{
	struct bgpd_avdtp *a = &audio->avdtp;
	uint64_t elapsed = now_ms() - audio->epoch + LEAD_MS;
	uint64_t due = elapsed * a->sbc.rate / 1000U / audio->frame_samples;
	size_t room = bgpd_avdtp_capacity(a) / audio->frame_size;

	if (room > MAX_FRAMES_PER_PACKET)
		room = MAX_FRAMES_PER_PACKET;
	if (!have_frame(audio) && audio->sent_any && due > audio->frames_sent) {
		/* The client is late: do not send a burst when it catches up.
		 */
		audio->epoch = now_ms();
		audio->frames_sent = 0;
		return;
	}
	while (due > audio->frames_sent && have_frame(audio)) {
		uint8_t packet[MAX_FRAMES_PER_PACKET * 256];
		int16_t pcm[1024];
		size_t used = 0;
		unsigned int count = 0;

		if (room == 0 || audio->frame_pcm > sizeof(pcm) ||
		    audio->frame_size > 256)
			return;
		while (count < room && due > audio->frames_sent + count &&
		       have_frame(audio)) {
			ssize_t written = 0;

			next_frame(audio, pcm);
			if (sbc_encode(&audio->sbc, pcm, audio->frame_pcm,
				       packet + used, sizeof(packet) - used,
				       &written) < 0 ||
			    written <= 0)
				return;
			used += (size_t)written;
			count++;
		}
		audio->frames_sent += count;
		audio->sent_any = true;
		if (bgpd_avdtp_send(a, packet, used, count,
				    count * audio->frame_samples) < 0)
			return;
	}
}

void bgpd_audio_poll(struct bgpd_audio *audio)
{
	struct bgpd_avdtp *a = &audio->avdtp;

	bgpd_avdtp_poll(a);
	if (a->changed) {
		snprintf(audio->status, sizeof(audio->status), "%s", a->status);
		audio->changed = true;
		a->changed = false;
	}
	if (audio->listener >= 0) {
		accept_client(audio);
		read_client(audio);
	}
	if (a->state == BGPD_AVDTP_IDLE) {
		finish_encoder(audio);
		audio->tone_ms = audio->tone_left = 0;
		drop_client(audio);
		audio->ring_head = audio->ring_used = 0;
		return;
	}
	if (a->state == BGPD_AVDTP_READY &&
	    (audio->tone_ms > 0 || audio->ring_used > 0))
		bgpd_avdtp_start(a);
	if (a->state != BGPD_AVDTP_STREAMING)
		return;
	if (!audio->encoding && start_encoder(audio) < 0) {
		snprintf(audio->status, sizeof(audio->status),
			 "Cannot set up the SBC encoder");
		audio->changed = true;
		bgpd_audio_close(audio);
		return;
	}
	stream(audio);
	if (audio->sent_any && audio->tone_left == 0 && audio->client < 0 &&
	    audio->ring_used < audio->frame_pcm) {
		finish_encoder(audio);
		/* The end of a frame left over would restart the stream. */
		audio->ring_head = audio->ring_used = 0;
		bgpd_avdtp_suspend(a);
	}
}
