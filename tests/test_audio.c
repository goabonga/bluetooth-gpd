/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * A test tone goes through the whole chain against a fake headset: the
 * negotiation, the SBC encoder, the pacing and the media packets, which
 * are decoded with libsbc to check the audio that comes out.
 */
#include "audio.h"

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
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

static const uint8_t headset[BGPD_ADDR_SIZE] = {
    1, 2, 3, 4, 5, 6, BGPD_ADDR_BREDR};

static struct bgpd_audio audio;
static int sink_sig, sink_media, channels[2], next_channel;

static int open_channel(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return channels[next_channel++];
}

/* Reads a command of the program and answers it, for the negotiation. */
static void serve_command(const uint8_t *answer, size_t size)
{
	struct pollfd p = {.fd = sink_sig, .events = POLLIN};
	uint8_t pdu[64], out[64];

	for (int i = 0; i < 100 && poll(&p, 1, 10) != 1; i++)
		bgpd_audio_poll(&audio);
	if (read(sink_sig, pdu, sizeof(pdu)) < 2) {
		CHECK(!"command expected");
		return;
	}
	out[0] = (uint8_t)((pdu[0] & 0xf0U) | 2U);
	out[1] = pdu[1];
	if (size > 0)
		memcpy(out + 2, answer, size);
	CHECK(write(sink_sig, out, size + 2) == (ssize_t)(size + 2));
	bgpd_audio_poll(&audio);
}

/* Accepts what the program asked of the headset, if anything. */
static void accept_pending(void)
{
	uint8_t pdu[64], out[2];

	while (read(sink_sig, pdu, sizeof(pdu)) >= 2) {
		out[0] = (uint8_t)((pdu[0] & 0xf0U) | 2U);
		out[1] = pdu[1];
		CHECK(write(sink_sig, out, 2) == 2);
	}
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* Reads the media packets the program sent and decodes them. */
static unsigned int decode_media(sbc_t *decoder, unsigned int *crossings,
				 int *peak)
{
	uint8_t packet[2048];
	int16_t pcm[4096];
	unsigned int frames = 0;
	ssize_t n;
	static int previous;

	while ((n = read(sink_media, packet, sizeof(packet))) > 0) {
		size_t offset = 13;
		unsigned int count = packet[12] & 0x0fU;

		while (count-- > 0 && offset < (size_t)n) {
			size_t written = 0;
			ssize_t used = sbc_decode(decoder, packet + offset,
						  (size_t)n - offset, pcm,
						  sizeof(pcm), &written);

			if (used <= 0 || written != 512)
				return frames;
			offset += (size_t)used;
			frames++;
			for (size_t i = 0; i + 1 < written / 2; i += 2) {
				if ((pcm[i] < 0) != (previous < 0))
					(*crossings)++;
				previous = pcm[i];
				if (pcm[i] > *peak)
					*peak = pcm[i];
			}
		}
	}
	return frames;
}

static int avrcp_end;

static int open_avrcp_none(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return -1;
}

static int open_avrcp(const uint8_t addr[BGPD_ADDR_SIZE])
{
	(void)addr;
	return avrcp_end;
}

/* When the headset has a hardware volume, no gain is applied to the audio. */
static void test_hardware_volume(void)
{
	static const uint8_t endpoints[] = {0x08, 0x08};
	static const uint8_t caps[] = {1, 0, 7, 6, 0, 0, 0x3f, 0xff, 2, 64};
	int sig[2], media[2], control[2];
	uint8_t frame[64];
	unsigned int frames = 0, crossings = 0;
	int peak = 0;
	sbc_t decoder;
	uint64_t start;
	ssize_t n;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sig) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, media) ==
	      0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, control) ==
	      0);
	channels[0] = sig[0];
	channels[1] = media[0];
	sink_sig = sig[1];
	sink_media = media[1];
	next_channel = 0;
	avrcp_end = control[0];
	bgpd_audio_init(&audio);
	audio.avdtp.open_channel = open_channel;
	audio.avrcp.open_channel = open_avrcp;
	CHECK(bgpd_audio_connect(&audio, headset, "Headset") == 0);
	bgpd_audio_poll(&audio);
	serve_command(endpoints, sizeof(endpoints));
	serve_command(caps, sizeof(caps));
	serve_command(NULL, 0);
	serve_command(NULL, 0);
	for (int i = 0; i < 4; i++)
		bgpd_audio_poll(&audio);
	CHECK(audio.avdtp.state == BGPD_AVDTP_READY);
	CHECK(bgpd_avrcp_active(&audio.avrcp));

	/* The headset answers the registration with its volume: 64/127. */
	n = read(control[1], frame, sizeof(frame));
	CHECK(n == 18 && frame[9] == 0x31);
	{
		uint8_t answer[15] = {(uint8_t)((frame[0] & 0xf0U) | 2U),
				      0x11,
				      0x0e,
				      0x0f,
				      0x48,
				      0x00,
				      0x00,
				      0x19,
				      0x58,
				      0x31,
				      0,
				      0,
				      2,
				      0x0d,
				      0x40};

		CHECK(write(control[1], answer, sizeof(answer)) ==
		      (ssize_t)sizeof(answer));
	}
	bgpd_audio_poll(&audio);
	CHECK(audio.hw_volume && audio.volume == 50);

	/* The volume is set on the headset: 30 % is 38/127. */
	bgpd_audio_set_volume(&audio, 30);
	n = read(control[1], frame, sizeof(frame));
	CHECK(n == 14 && frame[9] == 0x50 && frame[13] == 38);

	/* The audio is not attenuated: a tone keeps its level. */
	CHECK(bgpd_audio_tone(&audio, 100) == 0);
	CHECK(sbc_init(&decoder, 0) == 0);
	decoder.endian = SBC_LE;
	start = now_ms();
	for (int i = 0; i < 300 && now_ms() - start < 2000; i++) {
		bgpd_audio_poll(&audio);
		accept_pending();
		frames += decode_media(&decoder, &crossings, &peak);
		usleep(2000);
	}
	CHECK(frames > 30);
	CHECK(peak > 5000 && peak < 7000);

	sbc_finish(&decoder);
	bgpd_audio_close(&audio);
	CHECK(!audio.hw_volume);
	close(control[1]);
	close(sink_sig);
	close(sink_media);
}

/* A client whose greeting was read is given to the device. */
static void test_attach(void)
{
	static const uint8_t endpoints[] = {0x08, 0x08};
	static const uint8_t caps[] = {1, 0, 7, 6, 0, 0, 0x3f, 0xff, 2, 64};
	int sig[2], media[2], pair[2], second[2];
	uint8_t rate[4];
	int16_t pcm[2 * 441];
	unsigned int frames = 0, crossings = 0;
	int peak = 0;
	sbc_t decoder;
	uint64_t start;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sig) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, media) ==
	      0);
	channels[0] = sig[0];
	channels[1] = media[0];
	sink_sig = sig[1];
	sink_media = media[1];
	next_channel = 0;
	bgpd_audio_init(&audio);
	audio.avdtp.open_channel = open_channel;
	audio.avrcp.open_channel = open_avrcp_none;
	bgpd_audio_set_volume(&audio, 50);

	/* Not connected yet: refused with the rate 0. */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	CHECK(bgpd_audio_attach(&audio, pair[0]) == -1);
	CHECK(read(pair[1], rate, sizeof(rate)) == 4 && rate[0] == 0);
	CHECK(read(pair[1], rate, 1) == 0); /* and closed */
	close(pair[1]);

	CHECK(bgpd_audio_connect(&audio, headset, "Headset") == 0);
	bgpd_audio_poll(&audio);
	serve_command(endpoints, sizeof(endpoints));
	serve_command(caps, sizeof(caps));
	serve_command(NULL, 0);
	serve_command(NULL, 0);
	bgpd_audio_poll(&audio);
	CHECK(audio.avdtp.state == BGPD_AVDTP_READY);

	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
	CHECK(bgpd_audio_attach(&audio, pair[0]) == 0);
	CHECK(read(pair[1], rate, sizeof(rate)) == 4);
	CHECK((unsigned int)rate[0] + 256U * rate[1] + 65536U * rate[2] ==
	      44100);
	/* A second client is refused while the first one plays. */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, second) == 0);
	CHECK(bgpd_audio_attach(&audio, second[0]) == -1);
	CHECK(read(second[1], rate, sizeof(rate)) == 4 && rate[0] == 0);
	close(second[1]);

	for (int i = 0; i < 441; i++) {
		double angle = 2.0 * 3.14159265358979 * 1000.0 * i / 44100.0;

		pcm[(size_t)i * 2] = (int16_t)(8000.0 * sin(angle));
		pcm[(size_t)i * 2 + 1] = pcm[(size_t)i * 2];
	}
	for (int block = 0; block < 10; block++)
		CHECK(write(pair[1], pcm, sizeof(pcm)) == (ssize_t)sizeof(pcm));
	close(pair[1]);
	CHECK(sbc_init(&decoder, 0) == 0);
	decoder.endian = SBC_LE;
	start = now_ms();
	for (int i = 0; i < 300 && now_ms() - start < 2000; i++) {
		bgpd_audio_poll(&audio);
		accept_pending();
		frames += decode_media(&decoder, &crossings, &peak);
		usleep(2000);
	}
	/* 100 ms are 34 frames; at 50 % the gain is -25 dB: 8000 becomes
	 * about 440. */
	CHECK(frames >= 32 && frames <= 35);
	CHECK(peak > 380 && peak < 500);
	sbc_finish(&decoder);
	bgpd_audio_close(&audio);
	close(sink_sig);
	close(sink_media);
}

int main(void)
{
	static const uint8_t endpoints[] = {0x08, 0x08};
	static const uint8_t caps[] = {1, 0, 7, 6, 0, 0, 0x3f, 0xff, 2, 64};
	int sig[2], media[2];
	uint8_t packet[2048];
	int16_t pcm[4096];
	sbc_t decoder;
	uint64_t start;
	unsigned int frames = 0, packets = 0, crossings = 0;
	int peak = 0;
	int previous = 0;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sig) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, media) ==
	      0);
	channels[0] = sig[0];
	channels[1] = media[0];
	sink_sig = sig[1];
	sink_media = media[1];

	bgpd_audio_init(&audio);
	audio.avdtp.open_channel = open_channel;
	CHECK(bgpd_audio_tone(&audio, 200) == -1); /* not connected yet */
	CHECK(bgpd_audio_connect(&audio, headset, "Headset") == 0);
	CHECK(bgpd_audio_tone(&audio, 200) == 0);
	bgpd_audio_poll(&audio);
	serve_command(endpoints, sizeof(endpoints)); /* discover */
	serve_command(caps, sizeof(caps));	     /* capabilities */
	serve_command(NULL, 0);			     /* set configuration */
	serve_command(NULL, 0);			     /* open */
	bgpd_audio_poll(&audio);
	CHECK(audio.avdtp.state == BGPD_AVDTP_STARTING);
	serve_command(NULL, 0); /* start */
	CHECK(bgpd_audio_streaming(&audio));
	CHECK(strstr(audio.status, "Streaming") != NULL);

	CHECK(sbc_init(&decoder, 0) == 0);
	decoder.endian = SBC_LE;
	start = now_ms();
	while (bgpd_audio_streaming(&audio) && now_ms() - start < 3000) {
		ssize_t n;

		bgpd_audio_poll(&audio);
		while ((n = read(sink_media, packet, sizeof(packet))) > 0) {
			size_t offset = 13;
			unsigned int count = packet[12] & 0x0fU;

			CHECK(packet[0] == 0x80 && count > 0 && count <= 15);
			packets++;
			while (count-- > 0 && offset < (size_t)n) {
				size_t written = 0;
				ssize_t used =
				    sbc_decode(&decoder, packet + offset,
					       (size_t)n - offset, pcm,
					       sizeof(pcm), &written);

				CHECK(used > 0 && written == 512);
				if (used <= 0)
					break;
				offset += (size_t)used;
				frames++;
				for (size_t i = 0; i + 1 < written / 2;
				     i += 2) {
					int s = pcm[i];

					if ((s < 0) != (previous < 0))
						crossings++;
					previous = s;
					if (s > peak)
						peak = s;
				}
			}
			CHECK(offset == (size_t)n);
		}
		usleep(2000);
	}
	/* 200 ms at 44.1 kHz are 69 frames of 128 samples. */
	CHECK(frames >= 68 && frames <= 72);
	CHECK(packets > 1 && packets < frames);
	/* A 440 Hz sine crosses zero 880 times a second: 176 in 200 ms. */
	CHECK(crossings > 160 && crossings < 190);
	CHECK(peak > 5000 && peak < 7000);
	/* It was paced in real time, not sent at once. */
	CHECK(now_ms() - start >= 100);

	/* When the tone ends the stream is suspended. */
	for (int i = 0; i < 100 && audio.avdtp.state != BGPD_AVDTP_READY; i++) {
		serve_command(NULL, 0); /* suspend */
		bgpd_audio_poll(&audio);
	}
	CHECK(audio.avdtp.state == BGPD_AVDTP_READY);
	CHECK(!bgpd_audio_streaming(&audio));

	sbc_finish(&decoder);
	bgpd_audio_close(&audio);
	CHECK(!bgpd_audio_active(&audio));
	close(sink_sig);
	close(sink_media);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	test_hardware_volume();
	test_attach();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
