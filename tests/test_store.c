/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static char dir[256];
static struct bgpd_store a, b;
static struct bgpd_devices devices;

static void link_key(uint8_t *data, uint8_t first)
{
	memset(data, 0, 25);
	data[0] = first;
	data[6] = BGPD_ADDR_BREDR;
	data[7] = 4;
}

static void ltk_key(uint8_t *data, uint8_t first)
{
	memset(data, 0, 36);
	data[0] = first;
	data[6] = BGPD_ADDR_LE_PUBLIC;
	data[9] = 16;
}

static void test_default_dir(void)
{
	char path[128];

	setenv("XDG_STATE_HOME", "/state", 1);
	CHECK(bgpd_store_default_dir(path, sizeof(path)) == 0);
	CHECK(strcmp(path, "/state/bluetooth") == 0);
	setenv("XDG_STATE_HOME", "relative", 1);
	setenv("HOME", "/home/u", 1);
	CHECK(bgpd_store_default_dir(path, sizeof(path)) == 0);
	CHECK(strcmp(path, "/home/u/.local/state/bluetooth") == 0);
	CHECK(bgpd_store_default_dir(path, 8) == -1 && errno == ENAMETOOLONG);
}

static void test_add_and_reload(void)
{
	uint8_t link[25], ltk[36];

	link_key(link, 1);
	ltk_key(ltk, 2);
	CHECK(bgpd_store_open(&a, dir) == 0);
	CHECK(bgpd_store_load(&a, "ctl.keys") == 0 && a.count == 0);
	CHECK(bgpd_store_add(&a, BGPD_KEY_LINK, link, sizeof(link)) == 0);
	CHECK(bgpd_store_add(&a, BGPD_KEY_LTK, ltk, sizeof(ltk)) == 0);
	link[24] = 9; /* replaces the key of the same device */
	CHECK(bgpd_store_add(&a, BGPD_KEY_LINK, link, sizeof(link)) == 0);
	CHECK(a.count == 2);
	bgpd_store_close(&a);

	CHECK(bgpd_store_open(&b, dir) == 0);
	CHECK(bgpd_store_load(&b, "ctl.keys") == 0 && b.count == 2);
	CHECK(b.keys[0].data[24] == 9);
	bgpd_store_mark(&b, &devices);
	CHECK(devices.count == 2);
	CHECK(devices.items[0].paired && devices.items[0].saved);
	bgpd_store_close(&b);
}

static void test_lock(void)
{
	CHECK(bgpd_store_open(&a, dir) == 0);
	CHECK(bgpd_store_open(&b, dir) == -1 && errno == EWOULDBLOCK);
	bgpd_store_close(&a);
}

static void test_invalid(void)
{
	uint8_t link[25];

	link_key(link, 3);
	CHECK(bgpd_store_open(&a, dir) == 0);
	CHECK(bgpd_store_load(&a, "ctl.keys") == 0);
	CHECK(bgpd_store_add(&a, BGPD_KEY_LINK, link, 24) == -1);
	CHECK(errno == EINVAL);
	link[6] = BGPD_ADDR_LE_PUBLIC;
	CHECK(bgpd_store_add(&a, BGPD_KEY_LINK, link, 25) == -1);
	CHECK(bgpd_store_add(&a, 9, link, 25) == -1);
	CHECK(bgpd_store_add(&a, BGPD_KEY_LINK, link, 64) == -1);
	bgpd_store_close(&a);
}

static void test_forget(void)
{
	uint8_t addr[BGPD_ADDR_SIZE] = {1, 0, 0, 0, 0, 0, BGPD_ADDR_BREDR};

	CHECK(bgpd_store_open(&a, dir) == 0);
	CHECK(bgpd_store_load(&a, "ctl.keys") == 0 && a.count == 2);
	CHECK(bgpd_store_forget(&a, addr) == 0 && a.count == 1);
	bgpd_store_close(&a);
	CHECK(bgpd_store_open(&a, dir) == 0);
	CHECK(bgpd_store_load(&a, "ctl.keys") == 0 && a.count == 1);
	bgpd_store_close(&a);
}

static void test_corrupt_and_permissions(void)
{
	char path[300];
	int fd;

	snprintf(path, sizeof(path), "%s/bad.keys", dir);
	fd = open(path, O_WRONLY | O_CREAT, 0600);
	CHECK(fd >= 0 && write(fd, "garbage-garbage", 15) == 15);
	close(fd);
	CHECK(bgpd_store_open(&a, dir) == 0);
	CHECK(bgpd_store_load(&a, "bad.keys") == -1 && errno == EINVAL);
	CHECK(chmod(path, 0644) == 0);
	CHECK(bgpd_store_load(&a, "bad.keys") == -1 && errno == EPERM);
	bgpd_store_close(&a);

	CHECK(chmod(dir, 0755) == 0);
	CHECK(bgpd_store_open(&a, dir) == -1 && errno == EPERM);
	CHECK(chmod(dir, 0700) == 0);
}

static void cleanup(const char *root)
{
	char path[300];
	const char *files[] = {"nested/keys/ctl.keys", "nested/keys/bad.keys",
			       "nested/keys/.lock"};
	const char *dirs[] = {"nested/keys", "nested", ""};

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, files[i]);
		unlink(path);
	}
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
		rmdir(path);
	}
}

int main(void)
{
	char template[] = "/tmp/bgpd-store-XXXXXX";

	if (mkdtemp(template) == NULL)
		return 1;
	snprintf(dir, sizeof(dir), "%s/nested/keys", template);
	test_default_dir();
	test_add_and_reload();
	test_lock();
	test_invalid();
	test_forget();
	test_corrupt_and_permissions();
	cleanup(template);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
