/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "bgpd.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
	do {                                                           \
		if (!(cond)) {                                         \
			fprintf(stderr, "%s:%d: check failed: %s\n",   \
				__FILE__, __LINE__, #cond);            \
			failures++;                                    \
		}                                                      \
	} while (0)

static void test_version(void)
{
	unsigned major, minor, patch;
	int used = 0;

	CHECK(strcmp(bgpd_version(), BGPD_VERSION) == 0);
	CHECK(sscanf(bgpd_version(), "%u.%u.%u%n", &major, &minor, &patch,
		     &used) == 3);
	CHECK(used > 0 && bgpd_version()[used] == '\0');
}

int main(void)
{
	test_version();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
