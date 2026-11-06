/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */

#include "bgpd.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		puts(bgpd_version());
		return 0;
	}
	fputs("usage: bluetooth --version\n", stderr);
	return 2;
}
