/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "prof": the receive ring's profile of the other CPU (esp_sdr_prof.h). "prof dump" prints the
 * samples in the layout of Zephyr's "perf printbuf" (subsys/profiling/perf), so that the tools
 * reading perf's captures read these too: "Perf buf length <n>" and n words of 16 hex digits, three
 * a sample, the frame count 2, the program counter and the thread's frame. That frame is the
 * address of the thread's entry function, plus 2 for each thread before it in the dump's thread
 * table with the same entry function, plus 1 in an interrupt (0: a thread that has exited or did
 * not fit the table). zephyr/scripts/profiling/stackcollapse.py so files the samples under their
 * threads' entry functions; scripts/place/prof_map.py and hot_text.py name the threads from the
 * table printed before the samples, one line a thread:
 * "thread <index> <thread> <frame> <samples> <in interrupts> <name>".
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <esp_sdr/esp_sdr_prof.h>

#include "app_shell.h"

#define PROF_THREADS 24U

struct prof_thread {
	uint32_t th;
	uint32_t entry;
	uint32_t frame;
	uint32_t n, isr;
};

struct thread_find {
	uint32_t th;
	uint32_t entry;
};

static void thread_find(const struct k_thread *thread, void *user_data)
{
	struct thread_find *f = user_data;

	if ((uint32_t)(uintptr_t)thread == f->th) {
		f->entry = (uint32_t)(uintptr_t)thread->entry.pEntry;
	}
}

/* The threads of the samples, at most PROF_THREADS, with their entry functions and frames. */
static size_t thread_table(const struct esp_sdr_prof_info *in, const struct esp_sdr_prof_rec *s,
			   struct prof_thread *t)
{
	size_t nt = 0;

	for (uint32_t k = 0; k < in->n; k++) {
		uint32_t th = s[k].th & ~1U;
		size_t j;

		for (j = 0; j < nt && t[j].th != th; j++) {
		}
		if (j == nt) {
			if (nt == PROF_THREADS) {
				continue;
			}
			t[nt++] = (struct prof_thread){.th = th};
		}
		t[j].n++;
		t[j].isr += s[k].th & 1U;
	}
	for (size_t j = 0; j < nt; j++) {
		struct thread_find f = {.th = t[j].th};

		/* Only a live thread has an entry function and a name. */
		k_thread_foreach(thread_find, &f);
		t[j].entry = f.entry;
		t[j].frame = f.entry;
		for (size_t i = 0; i < j && f.entry != 0U; i++) {
			t[j].frame += t[i].entry == f.entry ? 2U : 0U;
		}
	}
	return nt;
}

/* The cadence, from the samples' cycle counts in the order taken. */
static void cadence(const struct shell *sh, const struct esp_sdr_prof_info *in,
		    const struct esp_sdr_prof_rec *s)
{
	const uint32_t cyc_us = sys_clock_hw_cycles_per_sec() / 1000000U;
	uint64_t span = 0;
	uint32_t longest = 0, over = 0;

	for (uint32_t k = 1; k < in->n; k++) {
		uint32_t d = s[k].t - s[k - 1U].t;

		span += d;
		longest = MAX(longest, d);
		over += d > in->period_cyc + in->period_cyc / 2U ? 1U : 0U;
	}
	shell_print(sh,
		    "prof: cpu %u, period %u us, span %u ms, missed %u, gaps over 1.5 periods %u, "
		    "longest %u us",
		    in->cpu, in->period_cyc / cyc_us, (uint32_t)(span / cyc_us / 1000U), in->missed,
		    over, longest / cyc_us);
}

static void dump(const struct shell *sh)
{
	struct prof_thread t[PROF_THREADS];
	struct esp_sdr_prof_info in;
	const struct esp_sdr_prof_rec *s = esp_sdr_prof_samples();
	uint32_t isr = 0;
	size_t nt;

	esp_sdr_prof_info(&in);
	nt = thread_table(&in, s, t);
	for (uint32_t k = 0; k < in.n; k++) {
		isr += s[k].th & 1U;
	}
	shell_print(sh, "prof: %u samples, %u in interrupts", in.n, isr);
	cadence(sh, &in, s);
	for (size_t j = 0; j < nt; j++) {
		const char *name = NULL;

		if (t[j].entry != 0U) {
			name = k_thread_name_get((k_tid_t)(uintptr_t)t[j].th);
		}
		shell_print(sh, "thread %u %08x %08x %6u %6u %s", (unsigned int)j, t[j].th,
			    t[j].frame, t[j].n, t[j].isr,
			    name != NULL && name[0] != '\0' ? name : "?");
	}
	shell_print(sh, "Perf buf length %u", 3U * in.n);
	for (uint32_t k = 0; k < in.n; k++) {
		uint32_t th = s[k].th & ~1U;
		uint32_t frame = 0;

		for (size_t j = 0; j < nt; j++) {
			if (t[j].th == th) {
				frame = t[j].frame;
				break;
			}
		}
		shell_print(sh, "%016x\n%016x\n%016x", 2U, s[k].pc, frame + (s[k].th & 1U));
	}
	shell_print(sh, "prof: end");
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t us = 64;

	if (argc > 1 && app_shell_u32(sh, argv[1], 1000000U, &us) != 0) {
		return -EINVAL;
	}
	if (esp_sdr_prof_start(us) != 0) {
		shell_error(sh, "period 1 to 1000000 us");
		return -EINVAL;
	}
	shell_print(sh, "prof: sampling every %u us while the ring runs", us);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_prof_info in;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	esp_sdr_prof_stop();
	esp_sdr_prof_info(&in);
	shell_print(sh, "prof: %u samples", in.n);
	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	esp_sdr_prof_stop();
	dump(sh);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_prof,
	SHELL_CMD_ARG(start, NULL, "[period_us]: clear and sample (default 64 us)", cmd_start, 1,
		      1),
	SHELL_CMD_ARG(stop, NULL, "stop sampling", cmd_stop, 1, 0),
	SHELL_CMD_ARG(dump, NULL, "stop and print the threads and the samples (perf's layout)",
		      cmd_dump, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(prof, &sub_prof, "The ring's profile of the other CPU (esp_sdr_prof.h)", NULL);
