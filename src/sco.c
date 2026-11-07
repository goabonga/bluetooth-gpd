/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "sco.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Kernel values, so that no BlueZ header is needed. */
#define AF_BLUETOOTH_FAMILY 31
#define BTPROTO_SCO 2
#define SOL_BLUETOOTH_LEVEL 274
#define SOL_SCO_LEVEL 17
#define SCO_OPTIONS_OPTION 1
#define BT_VOICE_OPTION 11
#define VOICE_CVSD_16BIT 0x0060U
#define VOICE_TRANSPARENT 0x0003U

#define CONNECT_TIMEOUT_MS 10000U
#define DEFAULT_MTU 48U
#define MSBC_FRAME 57U	  /* bytes of an mSBC frame */
#define MSBC_PACKET 60U	  /* with the H2 header and one byte of padding */
#define MSBC_SAMPLES 120U /* samples of an mSBC frame */
#define MSBC_PCM ((size_t)MSBC_SAMPLES * 2U)

struct sockaddr_sco {
	sa_family_t family;
	uint8_t addr[6];
};

struct sco_options {
	uint16_t mtu;
};

struct bt_voice {
	uint16_t setting;
};

/* The H2 header of an mSBC packet: 0x01 then a sequence number pattern. */
static const uint8_t h2_sequence[4] = {0x08, 0x38, 0xc8, 0xf8};

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int open_sco(const uint8_t addr[BGPD_ADDR_SIZE],
		    enum bgpd_hfp_codec codec)
{
	struct sockaddr_sco sa = {.family = AF_BLUETOOTH_FAMILY};
	struct bt_voice voice = {.setting = codec == BGPD_HFP_MSBC
						? VOICE_TRANSPARENT
						: VOICE_CVSD_16BIT};
	int fd =
	    socket(AF_BLUETOOTH_FAMILY,
		   SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_SCO);

	memcpy(sa.addr, addr, 6);
	if (fd >= 0 && (setsockopt(fd, SOL_BLUETOOTH_LEVEL, BT_VOICE_OPTION,
				   &voice, sizeof(voice)) < 0 ||
			(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
			 errno != EINPROGRESS))) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

void bgpd_sco_init(struct bgpd_sco *s)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->open_link = open_sco;
}

bool bgpd_sco_active(const struct bgpd_sco *s)
{
	return s->fd >= 0;
}

void bgpd_sco_close(struct bgpd_sco *s)
{
	if (s->sbc_ready) {
		sbc_finish(&s->encoder);
		sbc_finish(&s->decoder);
	}
	s->sbc_ready = false;
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
	s->opened = false;
}

int bgpd_sco_open(struct bgpd_sco *s, const uint8_t addr[BGPD_ADDR_SIZE],
		  enum bgpd_hfp_codec codec)
{
	if (bgpd_sco_active(s) || addr[6] != BGPD_ADDR_BREDR) {
		errno = EINVAL;
		return -1;
	}
	s->fd = s->open_link(addr, codec);
	if (s->fd < 0)
		return -1;
	s->codec = codec;
	s->rate = codec == BGPD_HFP_MSBC ? 16000 : 8000;
	s->mtu = DEFAULT_MTU;
	s->pending_size = s->outgoing_size = 0;
	s->mic_head = s->mic_used = s->speaker_head = s->speaker_used = 0;
	s->sequence = 0;
	s->deadline = now_ms() + CONNECT_TIMEOUT_MS;
	if (codec == BGPD_HFP_MSBC) {
		if (sbc_init_msbc(&s->encoder, 0) < 0 ||
		    sbc_init_msbc(&s->decoder, 0) < 0) {
			close(s->fd);
			s->fd = -1;
			errno = ENOMEM;
			return -1;
		}
		s->sbc_ready = true;
	}
	return 0;
}

/* ---- the rings ---- */

static void ring_put(int16_t *ring, size_t *head, size_t *used,
		     const int16_t *samples, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (*used == BGPD_SCO_RING) {
			/* Full: drop the oldest sample. */
			*head = (*head + 1U) % BGPD_SCO_RING;
			(*used)--;
		}
		ring[(*head + *used) % BGPD_SCO_RING] = samples[i];
		(*used)++;
	}
}

static size_t ring_take(const int16_t *ring, size_t *head, size_t *used,
			int16_t *samples, size_t count)
{
	size_t n = count < *used ? count : *used;

	for (size_t i = 0; i < n; i++)
		samples[i] = ring[(*head + i) % BGPD_SCO_RING];
	*head = (*head + n) % BGPD_SCO_RING;
	*used -= n;
	return n;
}

size_t bgpd_sco_read(struct bgpd_sco *s, int16_t *samples, size_t count)
{
	return ring_take(s->mic, &s->mic_head, &s->mic_used, samples, count);
}

size_t bgpd_sco_write(struct bgpd_sco *s, const int16_t *samples, size_t count)
{
	size_t room = BGPD_SCO_RING - s->speaker_used;
	size_t n = count < room ? count : room;

	ring_put(s->speaker, &s->speaker_head, &s->speaker_used, samples, n);
	return n;
}

/* ---- receiving ---- */

static void mic_put_bytes(struct bgpd_sco *s, const uint8_t *bytes, size_t size)
{
	int16_t samples[512];

	for (size_t offset = 0; offset + 1 < size;) {
		size_t n = 0;

		while (n < sizeof(samples) / sizeof(samples[0]) &&
		       offset + 1 < size) {
			samples[n++] =
			    (int16_t)(bytes[offset] |
				      (unsigned int)bytes[offset + 1] << 8U);
			offset += 2;
		}
		ring_put(s->mic, &s->mic_head, &s->mic_used, samples, n);
	}
}

/* Decodes the mSBC frames found in the bytes received. */
static void receive_msbc(struct bgpd_sco *s, const uint8_t *data, size_t size)
{
	size_t i = 0;

	while (i < size) {
		size_t room = sizeof(s->pending) - s->pending_size;
		size_t n = size - i < room ? size - i : room;

		memcpy(s->pending + s->pending_size, data + i, n);
		s->pending_size += n;
		i += n;
		/* Frames start with H2 (0x01, sequence) then the SBC sync. */
		while (s->pending_size >= MSBC_PACKET) {
			size_t at = 0;
			bool found = false;

			for (; at + 3 <= s->pending_size; at++) {
				if (s->pending[at] == 0x01 &&
				    (s->pending[at + 1] & 0x0fU) == 0x08U &&
				    s->pending[at + 2] == 0xad) {
					found = true;
					break;
				}
			}
			if (!found) {
				/* Keep the last two bytes: a header may start.
				 */
				size_t keep =
				    s->pending_size > 2 ? 2 : s->pending_size;

				memmove(s->pending,
					s->pending + s->pending_size - keep,
					keep);
				s->pending_size = keep;
				break;
			}
			if (at + 2 + MSBC_FRAME > s->pending_size) {
				memmove(s->pending, s->pending + at,
					s->pending_size - at);
				s->pending_size -= at;
				break;
			}
			{
				uint8_t pcm[MSBC_PCM * 2];
				size_t written = 0;
				ssize_t used = sbc_decode(
				    &s->decoder, s->pending + at + 2,
				    MSBC_FRAME, pcm, sizeof(pcm), &written);

				if (used > 0)
					mic_put_bytes(s, pcm, written);
			}
			at += 2 + MSBC_FRAME;
			memmove(s->pending, s->pending + at,
				s->pending_size - at);
			s->pending_size -= at;
		}
	}
}

/* ---- sending ---- */

/* Encodes the samples waiting for the earpiece into whole mSBC packets. */
static void encode_msbc(struct bgpd_sco *s)
{
	while (s->speaker_used >= MSBC_SAMPLES &&
	       s->outgoing_size + MSBC_PACKET <= sizeof(s->outgoing)) {
		int16_t samples[MSBC_SAMPLES];
		uint8_t *packet = s->outgoing + s->outgoing_size;
		ssize_t written = 0;

		ring_take(s->speaker, &s->speaker_head, &s->speaker_used,
			  samples, MSBC_SAMPLES);
		packet[0] = 0x01;
		packet[1] = h2_sequence[s->sequence++ % 4U];
		if (sbc_encode(&s->encoder, samples, MSBC_PCM, packet + 2,
			       MSBC_FRAME, &written) < 0 ||
		    written != (ssize_t)MSBC_FRAME)
			continue;
		packet[2 + MSBC_FRAME] = 0;
		s->outgoing_size += MSBC_PACKET;
	}
}

static void encode_cvsd(struct bgpd_sco *s)
{
	while (s->speaker_used > 0 &&
	       s->outgoing_size + 2 <= sizeof(s->outgoing)) {
		int16_t sample;

		ring_take(s->speaker, &s->speaker_head, &s->speaker_used,
			  &sample, 1);
		s->outgoing[s->outgoing_size++] =
		    (uint8_t)((uint16_t)sample & 0xffU);
		s->outgoing[s->outgoing_size++] =
		    (uint8_t)((uint16_t)sample >> 8U);
	}
}

/* Writes the encoded bytes in packets of the size of the link. */
static void flush(struct bgpd_sco *s)
{
	size_t unit = s->codec == BGPD_HFP_MSBC ? MSBC_PACKET : s->mtu;
	size_t sent = 0;

	while (s->outgoing_size - sent >= unit) {
		ssize_t n = write(s->fd, s->outgoing + sent, unit);

		if (n < 0 && errno == EINTR)
			continue;
		if (n != (ssize_t)unit)
			break; /* the controller is full: try again later */
		sent += unit;
	}
	memmove(s->outgoing, s->outgoing + sent, s->outgoing_size - sent);
	s->outgoing_size -= sent;
}

bool bgpd_sco_poll(struct bgpd_sco *s)
{
	uint8_t packet[512];

	if (s->fd < 0)
		return false;
	if (!s->opened) {
		struct pollfd p = {.fd = s->fd, .events = POLLOUT};
		int error = 0;
		socklen_t length = sizeof(error);

		if (poll(&p, 1, 0) == 1) {
			struct sco_options options;
			socklen_t size = sizeof(options);

			if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error,
				       &length) < 0 ||
			    error != 0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLERR) !=
				0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLHUP) !=
				0) {
				bgpd_sco_close(s);
				return false;
			}
			s->opened = true;
			if (getsockopt(s->fd, SOL_SCO_LEVEL, SCO_OPTIONS_OPTION,
				       &options, &size) == 0 &&
			    options.mtu >= 16 && options.mtu <= sizeof(packet))
				s->mtu = options.mtu;
		} else if (now_ms() > s->deadline) {
			bgpd_sco_close(s);
			return false;
		}
		return s->fd >= 0;
	}
	for (int i = 0; i < 64; i++) {
		ssize_t n = read(s->fd, packet, sizeof(packet));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			bgpd_sco_close(s);
			return false;
		}
		if (s->codec == BGPD_HFP_MSBC)
			receive_msbc(s, packet, (size_t)n);
		else
			mic_put_bytes(s, packet, (size_t)n);
	}
	if (s->codec == BGPD_HFP_MSBC)
		encode_msbc(s);
	else
		encode_cvsd(s);
	flush(s);
	return true;
}
