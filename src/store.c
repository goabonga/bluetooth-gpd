/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC_SIZE 9
#define HEADER_SIZE (MAGIC_SIZE + 2)
#define RECORD_SIZE (2 + BGPD_KEY_DATA_SIZE)

static const uint8_t magic[MAGIC_SIZE] = {'B', 'G', 'P', 'D', 'K',
					  'E', 'Y', '1', '\n'};

static int private_file(int fd)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -1;
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 077U) != 0 || st.st_nlink != 1) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

int bgpd_store_default_dir(char *buffer, size_t size)
{
	const char *base = getenv("XDG_STATE_HOME");
	const char *suffix = "/bluetooth";
	struct passwd *pw;
	int length;

	if (base == NULL || base[0] != '/') {
		base = getenv("HOME");
		if (base == NULL || base[0] != '/') {
			pw = getpwuid(geteuid());
			base = pw != NULL ? pw->pw_dir : NULL;
		}
		if (base == NULL || base[0] != '/') {
			errno = ENOENT;
			return -1;
		}
		suffix = "/.local/state/bluetooth";
	}
	length = snprintf(buffer, size, "%s%s", base, suffix);
	if (length < 0 || (size_t)length >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int make_parents(const char *directory)
{
	char path[PATH_MAX];
	size_t length = strlen(directory);

	if (length >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(path, directory, length + 1);
	for (char *p = path + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0700) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return 0;
}

int bgpd_store_open(struct bgpd_store *store, const char *dir)
{
	struct stat st;
	int saved;

	store->dirfd = store->lockfd = -1;
	store->count = 0;
	store->file[0] = '\0';
	if (dir == NULL || dir[0] == '\0') {
		errno = EINVAL;
		return -1;
	}
	if (make_parents(dir) < 0 || (mkdir(dir, 0700) < 0 && errno != EEXIST))
		return -1;
	store->dirfd =
	    open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (store->dirfd < 0)
		return -1;
	if (fstat(store->dirfd, &st) < 0)
		goto fail;
	if (st.st_uid != geteuid() || (st.st_mode & 077U) != 0) {
		errno = EPERM;
		goto fail;
	}
	store->lockfd = openat(store->dirfd, ".lock",
			       O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (store->lockfd < 0 || private_file(store->lockfd) < 0 ||
	    flock(store->lockfd, LOCK_EX | LOCK_NB) < 0)
		goto fail;
	return 0;
fail:
	saved = errno;
	bgpd_store_close(store);
	errno = saved;
	return -1;
}

void bgpd_store_close(struct bgpd_store *store)
{
	if (store->lockfd >= 0)
		close(store->lockfd);
	if (store->dirfd >= 0)
		close(store->dirfd);
	store->dirfd = store->lockfd = -1;
}

static void put16(uint8_t *p, uint16_t n)
{
	p[0] = (uint8_t)(n & 0xffU);
	p[1] = (uint8_t)(n >> 8U);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((unsigned int)p[1] << 8U));
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
	while (size > 0) {
		ssize_t n = write(fd, data, size);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		data += n;
		size -= (size_t)n;
	}
	return 0;
}

static int save(struct bgpd_store *store)
{
	char tmp[BGPD_STORE_FILE_SIZE + 32];
	uint8_t header[HEADER_SIZE];
	int fd, result, saved;

	if (store->dirfd < 0 || store->file[0] == '\0') {
		errno = EBADF;
		return -1;
	}
	snprintf(tmp, sizeof(tmp), ".%s.%ld.tmp", store->file, (long)getpid());
	fd = openat(store->dirfd, tmp,
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	memcpy(header, magic, MAGIC_SIZE);
	put16(header + MAGIC_SIZE, (uint16_t)store->count);
	result = write_all(fd, header, sizeof(header));
	for (size_t i = 0; i < store->count && result == 0; i++) {
		uint8_t record[RECORD_SIZE] = {0};

		record[0] = store->keys[i].kind;
		record[1] = store->keys[i].length;
		memcpy(record + 2, store->keys[i].data, store->keys[i].length);
		result = write_all(fd, record, sizeof(record));
	}
	if (result == 0)
		result = fsync(fd);
	saved = errno;
	if (close(fd) < 0 && result == 0) {
		result = -1;
		saved = errno;
	}
	if (result == 0 &&
	    renameat(store->dirfd, tmp, store->dirfd, store->file) < 0) {
		result = -1;
		saved = errno;
	}
	if (result == 0 && fsync(store->dirfd) < 0) {
		result = -1;
		saved = errno;
	}
	if (result < 0) {
		unlinkat(store->dirfd, tmp, 0);
		errno = saved;
	}
	return result;
}

static bool valid_key(const struct bgpd_key *key)
{
	const uint8_t *d = key->data;

	switch (key->kind) {
	case BGPD_KEY_LINK:
		return key->length == 25 && d[6] == BGPD_ADDR_BREDR &&
		       d[7] <= 8 && d[7] != 3 && d[24] <= 16;
	case BGPD_KEY_LTK:
		return key->length == 36 &&
		       (d[6] == BGPD_ADDR_LE_PUBLIC ||
			d[6] == BGPD_ADDR_LE_RANDOM) &&
		       d[7] <= 3 && d[8] <= 1 && d[9] >= 7 && d[9] <= 16;
	case BGPD_KEY_IRK:
		return key->length == 23 && (d[6] == BGPD_ADDR_LE_PUBLIC ||
					     d[6] == BGPD_ADDR_LE_RANDOM);
	default:
		return false;
	}
}

int bgpd_store_load(struct bgpd_store *store, const char *file)
{
	static uint8_t data[HEADER_SIZE + BGPD_MAX_KEYS * RECORD_SIZE + 1];
	size_t total = 0, count;
	int fd;

	store->count = 0;
	if (snprintf(store->file, sizeof(store->file), "%s", file) >=
	    (int)sizeof(store->file)) {
		store->file[0] = '\0';
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = openat(store->dirfd, store->file,
		    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	if (private_file(fd) < 0) {
		close(fd);
		return -1;
	}
	while (total < sizeof(data)) {
		ssize_t n = read(fd, data + total, sizeof(data) - total);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	close(fd);
	errno = EINVAL;
	if (total < HEADER_SIZE || memcmp(data, magic, MAGIC_SIZE) != 0)
		return -1;
	count = get16(data + MAGIC_SIZE);
	if (count > BGPD_MAX_KEYS || total != HEADER_SIZE + count * RECORD_SIZE)
		return -1;
	for (size_t i = 0; i < count; i++) {
		const uint8_t *r = data + HEADER_SIZE + i * RECORD_SIZE;
		struct bgpd_key key = {.kind = r[0], .length = r[1]};

		memcpy(key.data, r + 2, sizeof(key.data));
		if (!valid_key(&key))
			return -1;
		store->keys[store->count++] = key;
	}
	return 0;
}

void bgpd_store_mark(const struct bgpd_store *store,
		     struct bgpd_devices *devices)
{
	for (size_t i = 0; i < store->count; i++) {
		struct bgpd_device *device;

		if (store->keys[i].kind == BGPD_KEY_IRK)
			continue;
		device = bgpd_devices_get(devices, store->keys[i].data);
		if (device != NULL)
			device->paired = device->saved = true;
	}
}

int bgpd_store_add(struct bgpd_store *store, uint8_t kind, const uint8_t *data,
		   size_t length)
{
	struct bgpd_key key = {.kind = kind};
	size_t slot = store->count;

	if (length > BGPD_KEY_DATA_SIZE) {
		errno = EINVAL;
		return -1;
	}
	key.length = (uint8_t)length;
	memcpy(key.data, data, length);
	if (!valid_key(&key)) {
		errno = EINVAL;
		return -1;
	}
	for (size_t i = 0; i < store->count; i++) {
		const struct bgpd_key *k = &store->keys[i];

		if (k->kind == kind && memcmp(k->data, data, 7) == 0 &&
		    (kind != BGPD_KEY_LTK || k->data[8] == data[8])) {
			slot = i;
			break;
		}
	}
	if (slot == BGPD_MAX_KEYS) {
		errno = ENOSPC;
		return -1;
	}
	store->keys[slot] = key;
	if (slot == store->count)
		store->count++;
	return save(store);
}

int bgpd_store_forget(struct bgpd_store *store,
		      const uint8_t addr[BGPD_ADDR_SIZE])
{
	for (size_t i = 0; i < store->count;) {
		if (memcmp(store->keys[i].data, addr, BGPD_ADDR_SIZE) == 0)
			store->keys[i] = store->keys[--store->count];
		else
			i++;
	}
	return save(store);
}
