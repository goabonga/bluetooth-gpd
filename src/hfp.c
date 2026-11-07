/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "hfp.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Kernel values, so that no BlueZ header is needed. */
#define AF_BLUETOOTH_FAMILY 31
#define BTPROTO_RFCOMM 3
#define SOL_BLUETOOTH_LEVEL 274
#define BT_SECURITY_OPTION 4
#define BT_SECURITY_MEDIUM 2

#define CONNECT_TIMEOUT_MS 15000U
#define SLC_TIMEOUT_MS 15000U

/* Features of the headset (AT+BRSF) and of this audio gateway. */
#define HF_CODEC_NEGOTIATION (1U << 7U)
#define AG_CODEC_NEGOTIATION (1U << 9U)
#define GAIN_MAX 15U
#define GAIN_NONE ((unsigned int)-1)

struct sockaddr_rc {
	sa_family_t family;
	uint8_t addr[6];
	uint8_t channel;
};

struct bt_security_option {
	uint8_t level, key_size;
};

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void say(struct bgpd_hfp *h, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void say(struct bgpd_hfp *h, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(h->status, sizeof(h->status), format, args);
	va_end(args);
	h->changed = true;
}

static int open_rfcomm(const uint8_t addr[BGPD_ADDR_SIZE], uint8_t channel)
{
	struct sockaddr_rc sa = {.family = AF_BLUETOOTH_FAMILY,
				 .channel = channel};
	struct bt_security_option security = {.level = BT_SECURITY_MEDIUM};
	int fd =
	    socket(AF_BLUETOOTH_FAMILY,
		   SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_RFCOMM);

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

void bgpd_hfp_init(struct bgpd_hfp *h)
{
	memset(h, 0, sizeof(*h));
	h->fd = -1;
	h->speaker = h->microphone = GAIN_NONE;
	h->open_channel = open_rfcomm;
	bgpd_sdp_init(&h->sdp);
}

bool bgpd_hfp_active(const struct bgpd_hfp *h)
{
	return h->state != BGPD_HFP_IDLE;
}

void bgpd_hfp_close(struct bgpd_hfp *h)
{
	if (h->fd >= 0)
		close(h->fd);
	h->fd = -1;
	bgpd_sdp_close(&h->sdp);
	h->state = BGPD_HFP_IDLE;
	h->used = 0;
	h->features = 0;
	h->msbc = h->codec_selected = false;
	h->speaker = h->microphone = GAIN_NONE;
}

enum bgpd_hfp_codec bgpd_hfp_codec(const struct bgpd_hfp *h)
{
	return h->msbc && (h->features & HF_CODEC_NEGOTIATION) != 0
		   ? BGPD_HFP_MSBC
		   : BGPD_HFP_CVSD;
}

static void fail(struct bgpd_hfp *h, const char *what)
{
	say(h, "%s", what);
	bgpd_hfp_close(h);
}

/* Sends a result code or a response with its framing. */
static int reply(struct bgpd_hfp *h, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static int reply(struct bgpd_hfp *h, const char *format, ...)
{
	char text[BGPD_HFP_LINE];
	va_list args;
	int n;
	ssize_t written;

	va_start(args, format);
	n = vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (n < 0 || (size_t)n >= sizeof(text))
		return -1;
	written = write(h->fd, text, (size_t)n);
	return written == n ? 0 : -1;
}

#define OK "\r\nOK\r\n"
#define ERROR_REPLY "\r\nERROR\r\n"

int bgpd_hfp_select_codec(struct bgpd_hfp *h)
{
	if (h->state != BGPD_HFP_READY)
		return -1;
	if (bgpd_hfp_codec(h) != BGPD_HFP_MSBC) {
		h->codec_selected = true;
		return 0;
	}
	return reply(h, "\r\n+BCS: %d\r\n", BGPD_HFP_MSBC);
}

int bgpd_hfp_set_speaker(struct bgpd_hfp *h, unsigned int gain)
{
	if (h->state != BGPD_HFP_READY)
		return -1;
	h->speaker = gain > GAIN_MAX ? GAIN_MAX : gain;
	return reply(h, "\r\n+VGS: %u\r\n", h->speaker);
}

int bgpd_hfp_set_microphone(struct bgpd_hfp *h, unsigned int gain)
{
	if (h->state != BGPD_HFP_READY)
		return -1;
	h->microphone = gain > GAIN_MAX ? GAIN_MAX : gain;
	return reply(h, "\r\n+VGM: %u\r\n", h->microphone);
}

/* The number after "=" (first of a list), or -1. */
static long argument(const char *text)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	return end != text && errno == 0 ? value : -1;
}

static void on_command(struct bgpd_hfp *h, const char *at)
{
	long value;
	int rc = 0;

	if (strncmp(at, "AT+BRSF=", 8) == 0) {
		value = argument(at + 8);
		h->features = value >= 0 ? (unsigned int)value : 0;
		rc = reply(h, "\r\n+BRSF: %u\r\n" OK,
			   (h->features & HF_CODEC_NEGOTIATION) != 0
			       ? AG_CODEC_NEGOTIATION
			       : 0U);
	} else if (strncmp(at, "AT+BAC=", 7) == 0) {
		h->msbc = false;
		for (const char *c = at + 7; c != NULL; c = strchr(c, ',')) {
			if (*c == ',')
				c++;
			if (argument(c) == BGPD_HFP_MSBC)
				h->msbc = true;
		}
		rc = reply(h, OK);
	} else if (strcmp(at, "AT+CIND=?") == 0) {
		rc = reply(h, "\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),"
			      "(\"callsetup\",(0,3)),(\"callheld\",(0,2)),"
			      "(\"signal\",(0,5)),(\"roam\",(0,1)),"
			      "(\"battchg\",(0,5))\r\n" OK);
	} else if (strcmp(at, "AT+CIND?") == 0) {
		rc = reply(h, "\r\n+CIND: 1,0,0,0,5,0,5\r\n" OK);
	} else if (strncmp(at, "AT+CMER=", 8) == 0) {
		rc = reply(h, OK);
		if (h->state == BGPD_HFP_SLC) {
			h->state = BGPD_HFP_READY;
			h->deadline = 0;
			say(h, "Hands-free ready with %s (%s)", h->name,
			    bgpd_hfp_codec(h) == BGPD_HFP_MSBC ? "wide band"
							       : "narrow band");
		}
	} else if (strncmp(at, "AT+VGS=", 7) == 0 ||
		   strncmp(at, "AT+VGM=", 7) == 0) {
		value = argument(at + 7);
		if (value >= 0 && value <= (long)GAIN_MAX) {
			if (at[5] == 'S')
				h->speaker = (unsigned int)value;
			else
				h->microphone = (unsigned int)value;
			h->gain_changed = true;
		}
		rc = reply(h, OK);
	} else if (strncmp(at, "AT+BCS=", 7) == 0) {
		value = argument(at + 7);
		h->codec_selected = value == (long)bgpd_hfp_codec(h);
		rc = reply(h, h->codec_selected ? OK : ERROR_REPLY);
	} else if (strcmp(at, "AT+BCC") == 0) {
		/* The headset asks for the voice link: tell the codec. */
		rc = reply(h, OK);
		if (rc == 0)
			rc = bgpd_hfp_select_codec(h);
	} else if (strncmp(at, "AT+NREC=", 8) == 0 ||
		   strncmp(at, "AT+CLIP=", 8) == 0 ||
		   strncmp(at, "AT+CCWA=", 8) == 0 ||
		   strcmp(at, "AT+CHLD=?") == 0 ||
		   strncmp(at, "AT+XAPL=", 8) == 0) {
		rc = reply(h, OK);
	} else {
		rc = reply(h, ERROR_REPLY);
	}
	if (rc < 0)
		fail(h, "Lost the connection to the headset");
}

/* Feeds the bytes received, one AT command per carriage return. */
static void receive(struct bgpd_hfp *h, const char *data, size_t size)
{
	for (size_t i = 0; i < size && h->fd >= 0; i++) {
		if (data[i] == '\n')
			continue;
		if (data[i] != '\r') {
			if (h->used + 1 < sizeof(h->line))
				h->line[h->used++] = data[i];
			continue;
		}
		h->line[h->used] = '\0';
		h->used = 0;
		if (h->line[0] != '\0')
			on_command(h, h->line);
	}
}

int bgpd_hfp_attach(struct bgpd_hfp *h, int fd, const char *name)
{
	bgpd_hfp_close(h);
	h->fd = fd;
	snprintf(h->name, sizeof(h->name), "%s", name);
	h->state = BGPD_HFP_SLC;
	h->deadline = now_ms() + SLC_TIMEOUT_MS;
	return 0;
}

int bgpd_hfp_connect(struct bgpd_hfp *h, const uint8_t addr[BGPD_ADDR_SIZE],
		     const char *name)
{
	if (bgpd_hfp_active(h) ||
	    bgpd_sdp_start(&h->sdp, addr, BGPD_HFP_SERVICE_HF) < 0)
		return -1;
	memcpy(h->addr, addr, BGPD_ADDR_SIZE);
	snprintf(h->name, sizeof(h->name), "%s", name);
	h->state = BGPD_HFP_SDP;
	h->deadline = now_ms() + CONNECT_TIMEOUT_MS;
	return 0;
}

void bgpd_hfp_poll(struct bgpd_hfp *h)
{
	char data[256];

	if (h->state == BGPD_HFP_IDLE)
		return;
	if (h->state == BGPD_HFP_SDP && bgpd_sdp_poll(&h->sdp)) {
		if (h->sdp.state != BGPD_SDP_FOUND) {
			fail(h, "The device has no hands-free service");
			return;
		}
		h->fd = h->open_channel(h->addr, h->sdp.channel);
		if (h->fd < 0) {
			fail(h, "Cannot open the hands-free channel");
			return;
		}
		h->state = BGPD_HFP_CONNECTING;
		h->deadline = now_ms() + CONNECT_TIMEOUT_MS;
	}
	if (h->state == BGPD_HFP_CONNECTING) {
		struct pollfd p = {.fd = h->fd, .events = POLLOUT};
		int error = 0;
		socklen_t length = sizeof(error);

		if (poll(&p, 1, 0) == 1) {
			if (getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &error,
				       &length) < 0 ||
			    error != 0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLERR) !=
				0 ||
			    ((unsigned int)p.revents & (unsigned int)POLLHUP) !=
				0) {
				fail(h, "Cannot connect to the headset");
				return;
			}
			h->state = BGPD_HFP_SLC;
			h->deadline = now_ms() + SLC_TIMEOUT_MS;
		}
	}
	for (int i = 0;
	     i < 16 && h->fd >= 0 &&
	     (h->state == BGPD_HFP_SLC || h->state == BGPD_HFP_READY);
	     i++) {
		ssize_t n = read(h->fd, data, sizeof(data));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			say(h, "%s disconnected", h->name);
			bgpd_hfp_close(h);
			return;
		}
		receive(h, data, (size_t)n);
	}
	if (h->state != BGPD_HFP_IDLE && h->state != BGPD_HFP_READY &&
	    h->deadline != 0 && now_ms() > h->deadline)
		fail(h, "The headset did not complete the hands-free setup");
}
