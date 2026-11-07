/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

/*
 * The ALSA plugin loaded by ALSA in another process plays a tone into the
 * audio socket of the daemon code, which streams it to a fake headset; the
 * packets are decoded to check what came out. Usage: test_alsa PROGRAM
 * (the plugin is built next to PROGRAM).
 */
#include "audio.h"

#include <alsa/asoundlib.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
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

/* Answers a command of the negotiation, waiting for it. */
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

/* The program under test: opens the PCM named bt_test and plays a tone. */
static int play(const char *config, unsigned int milliseconds)
{
	snd_pcm_t *pcm;
	int16_t block[2 * 441];
	unsigned int rate = 44100;
	int err;

	setenv("ALSA_CONFIG_PATH", config, 1);
	err = snd_pcm_open(&pcm, "bt_test", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0)
		return 10;
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 0,
				 200000);
	if (err < 0)
		return 11;
	for (unsigned int b = 0; b < milliseconds / 10U; b++) {
		for (unsigned int i = 0; i < 441; i++) {
			double angle = 2.0 * 3.14159265358979 * 1000.0 *
				       (b * 441U + i) / 44100.0;

			block[(size_t)i * 2U] = (int16_t)(8000.0 * sin(angle));
			block[(size_t)i * 2U + 1U] = block[(size_t)i * 2U];
		}
		if (snd_pcm_writei(pcm, block, 441) < 0)
			return 12;
	}
	if (snd_pcm_drain(pcm) < 0)
		return 13;
	snd_pcm_close(pcm);
	return 0;
}

/* Waits for the child while serving the daemon side. */
static bool wait_child(pid_t child, int *code, sbc_t *decoder,
		       unsigned int *frames, unsigned int *crossings, int *peak)
{
	uint8_t packet[2048];
	int16_t pcm[4096];
	static int previous;
	uint64_t start = now_ms();
	int status = 0;
	bool exited = false;
	ssize_t n;

	while (now_ms() - start < 8000) {
		bgpd_audio_poll(&audio);
		accept_pending();
		while ((n = read(sink_media, packet, sizeof(packet))) > 0) {
			size_t offset = 13;
			unsigned int count = packet[12] & 0x0fU;

			while (count-- > 0 && offset < (size_t)n) {
				size_t written = 0;
				ssize_t used =
				    sbc_decode(decoder, packet + offset,
					       (size_t)n - offset, pcm,
					       sizeof(pcm), &written);

				if (used <= 0 || written != 512)
					break;
				offset += (size_t)used;
				(*frames)++;
				for (size_t i = 0; i + 1 < written / 2;
				     i += 2) {
					if ((pcm[i] < 0) != (previous < 0))
						(*crossings)++;
					previous = pcm[i];
					if (pcm[i] > *peak)
						*peak = pcm[i];
				}
			}
		}
		if (!exited && waitpid(child, &status, WNOHANG) == child) {
			*code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			exited = true;
		}
		/* The audio it left in the buffer is still being played. */
		if (exited && audio.ring_used < 512 &&
		    (audio.avdtp.state == BGPD_AVDTP_READY ||
		     audio.avdtp.state == BGPD_AVDTP_IDLE))
			return true;
		usleep(1000);
	}
	if (!exited) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
	}
	return false;
}

int main(int argc, char **argv)
{
	static const uint8_t endpoints[] = {0x08, 0x08};
	static const uint8_t caps[] = {1, 0, 7, 6, 0, 0, 0x3f, 0xff, 2, 64};
	char dir[] = "/tmp/bgpd-alsa-XXXXXX";
	char socket_path[100], config[160], plugin[PATH_MAX], program[PATH_MAX];
	char *directory;
	int sig[2], media[2], code = -1;
	unsigned int frames = 0, crossings = 0;
	int peak = 0;
	sbc_t decoder;
	pid_t child;
	FILE *f;

	if (argc != 2 || mkdtemp(dir) == NULL ||
	    realpath(argv[1], program) == NULL)
		return 2;
	directory = dirname(program);
	snprintf(plugin, sizeof(plugin), "%s/libasound_module_pcm_bluetooth.so",
		 directory);
	snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", dir);
	snprintf(config, sizeof(config), "%s/asound.conf", dir);
	f = fopen(config, "w");
	if (f == NULL)
		return 2;
	fprintf(f,
		"</usr/share/alsa/alsa.conf>\n"
		"pcm_type.bluetooth { lib \"%s\" }\n"
		"pcm.bt_test { type bluetooth server \"%s\" }\n",
		plugin, socket_path);
	fclose(f);

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sig) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, media) ==
	      0);
	channels[0] = sig[0];
	channels[1] = media[0];
	sink_sig = sig[1];
	sink_media = media[1];
	bgpd_audio_init(&audio);
	audio.avdtp.open_channel = open_channel;
	CHECK(bgpd_audio_listen(&audio, socket_path) == 0);
	CHECK(sbc_init(&decoder, 0) == 0);
	decoder.endian = SBC_LE;

	/* No device is ready: the PCM cannot be opened. */
	child = fork();
	if (child == 0)
		_exit(play(config, 100));
	CHECK(wait_child(child, &code, &decoder, &frames, &crossings, &peak));
	CHECK(code == 10);

	CHECK(bgpd_audio_connect(&audio, headset, "Headset") == 0);
	bgpd_audio_poll(&audio);
	serve_command(endpoints, sizeof(endpoints));
	serve_command(caps, sizeof(caps));
	serve_command(NULL, 0);
	serve_command(NULL, 0);
	bgpd_audio_poll(&audio);
	CHECK(audio.avdtp.state == BGPD_AVDTP_READY);

	/* An application plays 300 ms through the plugin. */
	child = fork();
	if (child == 0)
		_exit(play(config, 300));
	CHECK(wait_child(child, &code, &decoder, &frames, &crossings, &peak));
	CHECK(code == 0);
	/* 300 ms are 103 frames of 128 samples. */
	CHECK(frames >= 98 && frames <= 104);
	/* 1 kHz crosses zero 2000 times a second: 600 in 300 ms. */
	CHECK(crossings > 560 && crossings < 640);
	CHECK(peak > 7000 && peak < 9000);

	sbc_finish(&decoder);
	bgpd_audio_shutdown(&audio);
	close(sink_sig);
	close(sink_media);
	remove(config);
	rmdir(dir);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
