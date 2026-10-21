/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host side of the benchmark, built into the native simulator's runner with the host's libc: its
 * monotonic clock, and this process's core cycles in user mode where perf events are allowed,
 * summed over the core types of a hybrid CPU (a generic cycle event counts on one type only).
 */

#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int fds[2];
static int nfd = -1;

static long pmu_type(const char *name)
{
	char path[96];
	long type = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", name);
	f = fopen(path, "r");
	if (f != NULL) {
		if (fscanf(f, "%ld", &type) != 1) {
			type = -1;
		}
		fclose(f);
	}
	return type;
}

static void open_cycles(uint64_t config)
{
	struct perf_event_attr a;
	int fd;

	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.type = PERF_TYPE_HARDWARE;
	a.config = config;
	a.exclude_kernel = 1;
	a.exclude_hv = 1;
	fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
	if (fd >= 0 && nfd < 2) {
		fds[nfd++] = fd;
	}
}

uint64_t esdr_pfb_test_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000U + (uint64_t)t.tv_nsec;
}

/* Core cycles so far, 0 where they cannot be counted. */
uint64_t esdr_pfb_test_cycles(void)
{
	static const char *const pmus[] = {"cpu_core", "cpu_atom"};
	uint64_t sum = 0;

	if (nfd < 0) {
		nfd = 0;
		for (size_t i = 0; i < sizeof(pmus) / sizeof(pmus[0]); i++) {
			long type = pmu_type(pmus[i]);

			if (type >= 0) {
				open_cycles(PERF_COUNT_HW_CPU_CYCLES | ((uint64_t)type << 32));
			}
		}
		if (nfd == 0) {
			open_cycles(PERF_COUNT_HW_CPU_CYCLES);
		}
	}
	for (int i = 0; i < nfd; i++) {
		uint64_t v;

		if (read(fds[i], &v, sizeof(v)) == (ssize_t)sizeof(v)) {
			sum += v;
		}
	}
	return sum;
}
