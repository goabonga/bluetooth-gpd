/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "avdtp.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Kernel values, so that no BlueZ header is needed. */
#define AF_BLUETOOTH_FAMILY 31
#define BTPROTO_L2CAP 0
#define SOL_BLUETOOTH_LEVEL 274
#define SOL_L2CAP_LEVEL 6
#define L2CAP_OPTIONS_OPTION 1
#define BT_SECURITY_OPTION 4
#define BT_SECURITY_MEDIUM 2
#define AVDTP_PSM 25

#define STEP_TIMEOUT_MS 5000U
#define CONNECT_TIMEOUT_MS 15000U

struct sockaddr_l2cap {
	sa_family_t family;
	uint16_t psm;
	uint8_t addr[6];
	uint16_t cid;
	uint8_t addr_type;
};

struct bt_security_option {
	uint8_t level, key_size;
};

struct l2cap_options {
	uint16_t omtu, imtu, flush_to;
	uint8_t mode, fcs, max_tx;
	uint16_t txwin_size;
};

/* Signals, message types and capabilities. */
#define SIG_DISCOVER 0x01U
#define SIG_GET_CAPABILITIES 0x02U
#define SIG_SET_CONFIGURATION 0x03U
#define SIG_OPEN 0x06U
#define SIG_START 0x07U
#define SIG_CLOSE 0x08U
#define SIG_SUSPEND 0x09U
#define SIG_ABORT 0x0aU
#define MSG_COMMAND 0x00U
#define MSG_GENERAL_REJECT 0x01U
#define MSG_ACCEPT 0x02U
#define MSG_REJECT 0x03U
#define CAP_MEDIA_TRANSPORT 0x01U
#define CAP_MEDIA_CODEC 0x07U
#define CODEC_SBC 0x00U
#define MEDIA_AUDIO 0x00U
#define RTP_HEADER_SIZE 12U
#define PAYLOAD_TYPE 0x60

/* SBC codec information, first byte: sampling frequency and channel mode. */
#define SBC_RATE_48000 0x10U
#define SBC_RATE_44100 0x20U
#define SBC_MODE_JOINT 0x01U
#define SBC_MODE_STEREO 0x02U
#define SBC_MODE_DUAL 0x04U
#define SBC_MODE_MONO 0x08U
/* Second byte: block length, subbands, allocation method. */
#define SBC_BLOCKS_16 0x10U
#define SBC_SUBBANDS_8 0x04U
#define SBC_LOUDNESS 0x01U
#define SBC_SNR 0x02U
#define MAX_BITPOOL 53U

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void say(struct bgpd_avdtp *a, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void say(struct bgpd_avdtp *a, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(a->status, sizeof(a->status), format, args);
	va_end(args);
	a->changed = true;
	if (a->trace != NULL) {
		fprintf(a->trace, "%8llu ms avdtp %s\n",
			(unsigned long long)(now_ms() - a->trace_start),
			a->status);
		fflush(a->trace);
	}
}

static int open_l2cap(const uint8_t addr[BGPD_ADDR_SIZE])
{
	struct sockaddr_l2cap sa = {.family = AF_BLUETOOTH_FAMILY,
				    .psm = AVDTP_PSM};
	struct bt_security_option security = {.level = BT_SECURITY_MEDIUM};
	int fd = socket(AF_BLUETOOTH_FAMILY,
			SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
			BTPROTO_L2CAP);

	memcpy(sa.addr, addr, 6);
	if (fd >= 0 && (setsockopt(fd, SOL_BLUETOOTH_LEVEL, BT_SECURITY_OPTION,
				   &security, sizeof(security)) < 0 ||
			(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
			 errno != EINPROGRESS))) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

void bgpd_avdtp_init(struct bgpd_avdtp *a)
{
	memset(a, 0, sizeof(*a));
	a->sig = a->media = -1;
	a->open_channel = open_l2cap;
	a->mtu = BGPD_AVDTP_DEFAULT_MTU;
}

bool bgpd_avdtp_active(const struct bgpd_avdtp *a)
{
	return a->state != BGPD_AVDTP_IDLE;
}

void bgpd_avdtp_close(struct bgpd_avdtp *a)
{
	if (a->sig >= 0 && a->state >= BGPD_AVDTP_READY) {
		/* Politely, but without waiting for the answer. */
		uint8_t pdu[3] = {(uint8_t)(a->label << 4U), SIG_CLOSE,
				  (uint8_t)(a->seid << 2U)};
		ssize_t n = write(a->sig, pdu, sizeof(pdu));

		(void)n;
	}
	if (a->media >= 0)
		close(a->media);
	if (a->sig >= 0)
		close(a->sig);
	a->sig = a->media = -1;
	a->state = BGPD_AVDTP_IDLE;
}

static void fail(struct bgpd_avdtp *a, const char *what)
{
	say(a, "%s", what);
	bgpd_avdtp_close(a);
}

static int send_signal(struct bgpd_avdtp *a, uint8_t signal,
		       const uint8_t *payload, size_t size)
{
	uint8_t pdu[64];
	ssize_t n;

	if (size + 2 > sizeof(pdu))
		return -1;
	a->label = (uint8_t)((a->label + 1U) & 0x0fU);
	a->signal = signal;
	pdu[0] = (uint8_t)(a->label << 4U); /* single packet, command */
	pdu[1] = signal;
	if (size > 0)
		memcpy(pdu + 2, payload, size);
	do {
		n = write(a->sig, pdu, size + 2);
	} while (n < 0 && errno == EINTR);
	if (n != (ssize_t)(size + 2)) {
		fail(a, "Lost the connection to the audio device");
		return -1;
	}
	a->deadline = now_ms() + STEP_TIMEOUT_MS;
	if (a->trace != NULL) {
		fprintf(a->trace, "%8llu ms avdtp > signal 0x%02x\n",
			(unsigned long long)(now_ms() - a->trace_start),
			signal);
		fflush(a->trace);
	}
	return 0;
}

/* Where the sink can be asked about its next endpoint, or gives up. */
static void ask_capabilities(struct bgpd_avdtp *a)
{
	uint8_t seid;

	if (a->sep_next >= a->sep_count) {
		fail(a, "The device has no usable SBC audio endpoint");
		return;
	}
	seid = (uint8_t)(a->seps[a->sep_next] << 2U);
	a->state = BGPD_AVDTP_CAPABILITIES;
	send_signal(a, SIG_GET_CAPABILITIES, &seid, 1);
}

/* Chooses the best SBC configuration the sink supports. */
static bool choose(struct bgpd_avdtp *a, const uint8_t info[4])
{
	unsigned int min = info[2], max = info[3];

	if ((info[0] & SBC_RATE_44100) != 0)
		a->sbc.rate = 44100;
	else if ((info[0] & SBC_RATE_48000) != 0)
		a->sbc.rate = 48000;
	else
		return false;
	if ((info[0] & SBC_MODE_JOINT) != 0)
		a->sbc.channel_mode = SBC_MODE_JOINT;
	else if ((info[0] & SBC_MODE_STEREO) != 0)
		a->sbc.channel_mode = SBC_MODE_STEREO;
	else if ((info[0] & SBC_MODE_DUAL) != 0)
		a->sbc.channel_mode = SBC_MODE_DUAL;
	else
		return false;
	/* Every sink must support 16 blocks, 8 subbands and loudness. */
	if ((info[1] & SBC_BLOCKS_16) == 0 || (info[1] & SBC_SUBBANDS_8) == 0 ||
	    (info[1] & (SBC_LOUDNESS | SBC_SNR)) == 0 || min > max)
		return false;
	a->sbc.bitpool = (uint8_t)(max < MAX_BITPOOL ? max : MAX_BITPOOL);
	if (a->sbc.bitpool < min)
		a->sbc.bitpool = (uint8_t)min;
	a->sbc.loudness = (info[1] & SBC_LOUDNESS) != 0;
	return true;
}

static void configure(struct bgpd_avdtp *a)
{
	uint8_t payload[2 + 2 + 2 + 6];

	payload[0] = (uint8_t)(a->seid << 2U);
	payload[1] = (uint8_t)(1U << 2U); /* our endpoint */
	payload[2] = CAP_MEDIA_TRANSPORT;
	payload[3] = 0;
	payload[4] = CAP_MEDIA_CODEC;
	payload[5] = 6;
	payload[6] = (uint8_t)(MEDIA_AUDIO << 4U);
	payload[7] = CODEC_SBC;
	payload[8] =
	    (uint8_t)((a->sbc.rate == 44100 ? SBC_RATE_44100 : SBC_RATE_48000) |
		      a->sbc.channel_mode);
	payload[9] = (uint8_t)(SBC_BLOCKS_16 | SBC_SUBBANDS_8 |
			       (a->sbc.loudness ? SBC_LOUDNESS : SBC_SNR));
	payload[10] = a->sbc.bitpool;
	payload[11] = a->sbc.bitpool;
	a->state = BGPD_AVDTP_CONFIGURE;
	send_signal(a, SIG_SET_CONFIGURATION, payload, sizeof(payload));
}

static void on_discover(struct bgpd_avdtp *a, const uint8_t *p, size_t size)
{
	a->sep_count = a->sep_next = 0;
	for (; size >= 2 && a->sep_count < BGPD_AVDTP_MAX_SEPS;
	     p += 2, size -= 2) {
		bool in_use = (p[0] & 0x02U) != 0;
		bool audio = (p[1] >> 4U) == MEDIA_AUDIO;
		bool sink = (p[1] & 0x08U) != 0;

		if (!in_use && audio && sink)
			a->seps[a->sep_count++] = p[0] >> 2U;
	}
	ask_capabilities(a);
}

static void on_capabilities(struct bgpd_avdtp *a, const uint8_t *p, size_t size)
{
	while (size >= 2 && size >= 2 + (size_t)p[1]) {
		size_t length = p[1];

		if (p[0] == CAP_MEDIA_CODEC && length == 6 &&
		    (p[2] >> 4U) == MEDIA_AUDIO && p[3] == CODEC_SBC &&
		    choose(a, p + 4)) {
			a->seid = a->seps[a->sep_next];
			configure(a);
			return;
		}
		p += 2 + length;
		size -= 2 + length;
	}
	a->sep_next++;
	ask_capabilities(a);
}

static void connected_media(struct bgpd_avdtp *a)
{
	struct l2cap_options options;
	socklen_t length = sizeof(options);

	a->mtu = BGPD_AVDTP_DEFAULT_MTU;
	if (getsockopt(a->media, SOL_L2CAP_LEVEL, L2CAP_OPTIONS_OPTION,
		       &options, &length) == 0 &&
	    options.omtu > RTP_HEADER_SIZE + 1U + 100U)
		a->mtu = options.omtu;
	a->state = BGPD_AVDTP_READY;
	a->deadline = 0;
	say(a, "Audio ready: SBC %u Hz, bitpool %u", a->sbc.rate,
	    a->sbc.bitpool);
}

/* 1 when the channel is connected, 0 while it connects, -1 when it failed. */
static int channel_state(int fd, const char **why)
{
	struct pollfd p = {.fd = fd, .events = POLLOUT};
	int error = 0;
	socklen_t length = sizeof(error);

	if (poll(&p, 1, 0) != 1)
		return 0;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
	    error != 0 ||
	    ((unsigned int)p.revents & (unsigned int)POLLERR) != 0 ||
	    ((unsigned int)p.revents & (unsigned int)POLLHUP) != 0) {
		*why = error != 0 ? strerror(error) : "closed";
		return -1;
	}
	return 1;
}

int bgpd_avdtp_connect(struct bgpd_avdtp *a, const uint8_t addr[BGPD_ADDR_SIZE],
		       const char *name)
{
	int fd;

	if (addr[6] != BGPD_ADDR_BREDR || bgpd_avdtp_active(a)) {
		errno = EINVAL;
		return -1;
	}
	fd = a->open_channel(addr);
	if (fd < 0) {
		say(a, "Cannot connect: %s", strerror(errno));
		return -1;
	}
	a->sig = fd;
	memcpy(a->addr, addr, BGPD_ADDR_SIZE);
	snprintf(a->name, sizeof(a->name), "%s", name);
	a->state = BGPD_AVDTP_CONNECTING;
	a->seq = 0;
	a->timestamp = 0;
	a->ssrc = (uint32_t)now_ms() * 2654435761U;
	a->deadline = now_ms() + CONNECT_TIMEOUT_MS;
	say(a, "Connecting to %s...", name);
	return 0;
}

static void on_response(struct bgpd_avdtp *a, bool accepted, const uint8_t *p,
			size_t size)
{
	if (!accepted) {
		/* The sink refused: the error code tells why. */
		if (a->state == BGPD_AVDTP_CAPABILITIES) {
			a->sep_next++;
			ask_capabilities(a);
			return;
		}
		if (a->state == BGPD_AVDTP_STARTING)
			a->state = BGPD_AVDTP_READY;
		else if (a->state == BGPD_AVDTP_SUSPENDING)
			a->state = BGPD_AVDTP_STREAMING;
		else {
			fail(a, "The audio device refused the request");
			return;
		}
		say(a, "The audio device refused the request (error 0x%02x)",
		    size > 0 ? p[size - 1] : 0);
		return;
	}
	switch (a->state) {
	case BGPD_AVDTP_DISCOVER:
		on_discover(a, p, size);
		break;
	case BGPD_AVDTP_CAPABILITIES:
		on_capabilities(a, p, size);
		break;
	case BGPD_AVDTP_CONFIGURE: {
		uint8_t seid = (uint8_t)(a->seid << 2U);

		a->state = BGPD_AVDTP_OPEN;
		send_signal(a, SIG_OPEN, &seid, 1);
		break;
	}
	case BGPD_AVDTP_OPEN:
		a->media = a->open_channel(a->addr);
		if (a->media < 0) {
			fail(a, "Cannot open the audio channel");
			break;
		}
		a->state = BGPD_AVDTP_MEDIA;
		a->deadline = now_ms() + CONNECT_TIMEOUT_MS;
		break;
	case BGPD_AVDTP_STARTING:
		a->state = BGPD_AVDTP_STREAMING;
		a->deadline = 0;
		say(a, "Streaming to %s", a->name);
		break;
	case BGPD_AVDTP_SUSPENDING:
		a->state = BGPD_AVDTP_READY;
		a->deadline = 0;
		say(a, "Audio stopped");
		break;
	default:
		break;
	}
}

/* A command of the sink: closing is honoured, the rest is not supported. */
static void on_command(struct bgpd_avdtp *a, uint8_t label, uint8_t signal)
{
	uint8_t pdu[2] = {(uint8_t)(label << 4U), signal};
	ssize_t n;

	if (signal == SIG_CLOSE || signal == SIG_ABORT)
		pdu[0] = (uint8_t)(pdu[0] | MSG_ACCEPT);
	else
		pdu[0] = (uint8_t)(pdu[0] | MSG_GENERAL_REJECT);
	n = write(a->sig, pdu, sizeof(pdu));
	(void)n;
	if (signal == SIG_CLOSE || signal == SIG_ABORT) {
		say(a, "%s closed the audio connection", a->name);
		bgpd_avdtp_close(a);
	}
}

static void on_pdu(struct bgpd_avdtp *a, const uint8_t *pdu, size_t size)
{
	uint8_t type, label, signal;

	if (size < 2 || (((unsigned int)pdu[0] >> 2U) & 3U) != 0)
		return; /* fragmented packets are not used by the commands */
	type = pdu[0] & 3U;
	label = pdu[0] >> 4U;
	signal = pdu[1] & 0x3fU;
	if (type == MSG_COMMAND) {
		on_command(a, label, signal);
		return;
	}
	if (label != a->label || signal != a->signal)
		return;
	on_response(a, type == MSG_ACCEPT, pdu + 2, size - 2);
}

void bgpd_avdtp_poll(struct bgpd_avdtp *a)
{
	uint8_t pdu[1024];
	const char *why = "";

	if (a->state == BGPD_AVDTP_IDLE)
		return;
	if (a->state == BGPD_AVDTP_CONNECTING) {
		int r = channel_state(a->sig, &why);

		if (r < 0) {
			say(a, "Connection failed: %s", why);
			bgpd_avdtp_close(a);
			return;
		}
		if (r > 0) {
			a->state = BGPD_AVDTP_DISCOVER;
			say(a, "Connected; looking for the audio endpoints...");
			send_signal(a, SIG_DISCOVER, NULL, 0);
		}
	} else if (a->state == BGPD_AVDTP_MEDIA) {
		int r = channel_state(a->media, &why);

		if (r < 0) {
			say(a, "Audio channel failed: %s", why);
			bgpd_avdtp_close(a);
			return;
		}
		if (r > 0)
			connected_media(a);
	}
	for (int i = 0;
	     i < 16 && a->sig >= 0 && a->state != BGPD_AVDTP_CONNECTING; i++) {
		ssize_t n = read(a->sig, pdu, sizeof(pdu));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			say(a, "%s disconnected", a->name);
			bgpd_avdtp_close(a);
			return;
		}
		on_pdu(a, pdu, (size_t)n);
	}
	if (a->sig >= 0 && a->deadline != 0 && now_ms() > a->deadline &&
	    a->state != BGPD_AVDTP_READY && a->state != BGPD_AVDTP_STREAMING)
		fail(a, "The audio device did not answer");
}

int bgpd_avdtp_start(struct bgpd_avdtp *a)
{
	uint8_t seid = (uint8_t)(a->seid << 2U);

	if (a->state != BGPD_AVDTP_READY)
		return -1;
	a->state = BGPD_AVDTP_STARTING;
	return send_signal(a, SIG_START, &seid, 1);
}

int bgpd_avdtp_suspend(struct bgpd_avdtp *a)
{
	uint8_t seid = (uint8_t)(a->seid << 2U);

	if (a->state != BGPD_AVDTP_STREAMING)
		return -1;
	a->state = BGPD_AVDTP_SUSPENDING;
	return send_signal(a, SIG_SUSPEND, &seid, 1);
}

size_t bgpd_avdtp_capacity(const struct bgpd_avdtp *a)
{
	return a->mtu > RTP_HEADER_SIZE + 1U ? a->mtu - RTP_HEADER_SIZE - 1U
					     : 0;
}

int bgpd_avdtp_send(struct bgpd_avdtp *a, const uint8_t *frames, size_t size,
		    unsigned int count, unsigned int samples)
{
	uint8_t packet[65536];
	ssize_t n;

	if (a->state != BGPD_AVDTP_STREAMING || count == 0 || count > 15 ||
	    size > bgpd_avdtp_capacity(a) ||
	    size + RTP_HEADER_SIZE + 1U > sizeof(packet))
		return -1;
	packet[0] = 0x80;
	packet[1] = PAYLOAD_TYPE;
	packet[2] = (uint8_t)(a->seq >> 8U);
	packet[3] = (uint8_t)a->seq;
	for (unsigned int i = 0; i < 4; i++) {
		packet[4 + i] = (uint8_t)(a->timestamp >> (24U - 8U * i));
		packet[8 + i] = (uint8_t)(a->ssrc >> (24U - 8U * i));
	}
	packet[RTP_HEADER_SIZE] = (uint8_t)count;
	memcpy(packet + RTP_HEADER_SIZE + 1U, frames, size);
	do {
		n = write(a->media, packet, size + RTP_HEADER_SIZE + 1U);
	} while (n < 0 && errno == EINTR);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return -1; /* the sink is slower than the audio: drop it */
	if (n != (ssize_t)(size + RTP_HEADER_SIZE + 1U)) {
		fail(a, "Lost the audio channel");
		return -1;
	}
	a->seq++;
	a->timestamp += samples;
	return 0;
}
