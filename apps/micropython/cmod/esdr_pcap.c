/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The packet capture of the Pcap app (esdr_pcap_core.h: apps/pcap's channel mode and decoders)
 * and apps/pcap's pcapng writer, in the esdr module:
 *
 *   pcap_new(freq, rate=2000000, decoders=PCAP_DEC_DEFAULT, ratio=4.0, gain=255, shift=0)
 *                              -> state (a bytearray): one channel at freq (Hz) and rate (2 or
 *                              4 MS/s); shift scales the samples to a full scale of 16384
 *   pcap_tune(state, freq, gain=255)
 *                              another channel: an open burst ends, the floor is learnt again
 *   pcap_feed(state, buf, start, n, at, analyse=1) -> [packet]
 *                              n samples of buf (int16 I, Q) from sample start, the first of
 *                              them sample at of the stream (another index than the last
 *                              call's end is a gap); at most analyse bursts no decoder claimed
 *                              get the generic analysis
 *   pcap_flush(state, analyse=1) -> [packet]
 *                              the open burst ends (the capture stops), queued bursts decoded
 *   pcap_stats(state) -> dict  esdr_pcap_stats (the decoders' times as ble_us, wpan_us,
 *                              generic_us), and calls and busy_us of pcap_feed() and
 *                              pcap_take(); with the engine det_us (its detector's time),
 *                              lapped (it fell a stage behind the radio), bad (samples
 *                              overwritten while it read them), pending
 *
 * With the engine (esdr_pcap.h's ESDR_PCAP_ENGINE) a thread above MicroPython runs the detector
 * where the radio's samples land, and a second one, at MicroPython's priority, the decoders,
 * their packets into a ring in the state for pcap_take(). With the ring stage (the S3 in
 * multiapp, the S31) the radio receives into internal RAM (the S3's SRAM bank 2:
 * esdr_rx_start_stage(); the S31's capture bank: esdr_remote_rx_start_stage()) from CPU 1, the
 * threads run on CPU 0. On the P4 the link's parser fills a ring in the heap
 * (esdr_remote_rx_start() with wrap) and the detector reads it in place
 * (esdr_remote_rx_peek()), below the link's threads and USB; with two CPUs (the WHY2025 badge)
 * both threads run on CPU 1, MicroPython, the link and USB on CPU 0:
 *
 *   pcap_new(..., pool=32)     the bursts the detector can hand over (2 to 128), and the ring
 *   pcap_start(state[, ring])  the radio and both threads at the state's rate (2 or 4 MS/s);
 *                              on the P4 into ring (int16 I, Q, PCAP_RING_MIN samples at least),
 *                              kept from the garbage collector until pcap_stop()
 *   pcap_take(state, n=64) -> [packet]
 *                              up to n packets the decoders made (without the engine: up to n
 *                              closed bursts decoded here, pcap_take(state, n, analyse))
 *   pcap_take_pkts(state, n, cls, no, t0, at0, rate, freq) -> [cls]
 *                              as pcap_take(), each packet made an object of the egg's
 *                              pcap_view.Pkt with the dissector's fields too (in one
 *                              allocation of its attribute map), numbered from no + 1, at
 *                              t0 + (at - at0) / rate seconds
 *   pcap_pending(state) -> int closed bursts waiting
 *   pcap_stop(state)           threads and radio stop, the open burst is closed (pcap_flush()
 *                              then returns the ring's rest and decodes what is left)
 *   pcap_stats(state)          also dec_us (the decoders' thread), ring_lost (packets without
 *                              room), ring_waits (the decoders waited for pcap_take()), shed
 *                              (burst records left out while the ring was half full), det_cpu
 *                              and dec_cpu (the CPU each thread ran on, -1 before it ran)
 *     packet: (iface, at, ok, power_x10, floor_x10, offset_hz, duration_us, record, comment)
 *                              iface PCAP_IF_*, record the pcapng packet's payload, comment a
 *                              str or None
 *   pcap_dissect(iface, record) -> (channel, phy, kind, src, dst, random, info, length, ok)
 *                              the table's fields of a packet (eggs/pcap/pcap_view.py's
 *                              dissector, in C); ok None unless the record carries its CRC's
 *                              verdict
 *   pcapng_head(hardware, os, appl) -> bytes
 *                              a section header and apps/pcap's three interfaces
 *   pcapng_epb(iface, t_ns, record, comment=None) -> bytes
 *                              an enhanced packet block, inbound, nanoseconds
 *   PCAP_DEC_DEFAULT, PCAP_DEC_BAD_CRC
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <esp_sdr/esp_sdr_place.h>

#include "py/mphal.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objtype.h"
#include "py/runtime.h"

#include "esdr_pcap.h"
#include "esdr_pcap_core.h"
#include "esdr_pcap_dissect.h"
#include "pcap_proto.h"

#if defined(ESDR_PCAP_ENGINE_STAGE)
#include <esp_sdr/esp_sdr_ring_stage.h>

#if defined(CONFIG_ESP_SDR_RING)
/* The S3: the ring's run into bank 2 (esdr_rx.c). */
#include "esdr_rx.h"
#else
/* The S31: the dump ring's gapless stream into the capture bank (esdr_local.c). */
#include "esdr_defs.h"
#include "esdr_remote.h"
#endif
#elif defined(ESDR_PCAP_ENGINE_LINK)
/* The P4: the link's receive ring (esdr_remote.c). */
#include "esdr_defs.h"
#include "esdr_remote.h"
#endif

#define PCAP_MAGIC 0x50434150U

/* The bytearray: this header, then the core's memory (esdr_pcap_size() bytes, 16-aligned). */
struct pcap_obj {
	uint32_t magic, calls;
	/* pcap_feed()'s and pcap_take()'s time, the engine's detector's, in cycles (each call's
	 * delta is wrap-safe). */
	uint64_t busy_cycles, det_cycles;
	/* The engine's reader fell a stage behind; samples overwritten while it read them. */
	uint32_t lapped, bad;
	size_t size;
	struct esdr_pcap *core;
	/* The pool's buffers (pcap_new()'s pool). */
	uint32_t pool;
	/*
	 * The engine's decoder thread: its time, packets it found no room for in the ring, and
	 * times it waited for room. The packet ring (ring_size bytes after the core's memory, a
	 * power of 2; none without a pool): wr is the thread's, rd pcap_take()'s, both byte counts
	 * modulo 2^32.
	 */
	uint64_t dec_cycles;
	uint32_t ring_lost, ring_waits, shed;
	/* The CPUs the engine's detector and decoders ran on (-1: not yet). */
	int8_t det_cpu, dec_cpu;
	uint8_t *ring;
	uint32_t ring_size;
	uint32_t wr, rd;
};

/* The state the engine runs on, kept from the garbage collector while it does. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_pcap_running);
#ifdef ESDR_PCAP_ENGINE_LINK
/* The receive ring it reads (the P4), likewise: its own root, not esdr_rx_buf, which
 * esdr.rx_stop() and rx_wait() let go. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_pcap_ring);
#endif

static struct pcap_obj *pcap_of(mp_obj_t st)
{
	mp_buffer_info_t bi;
	struct pcap_obj *o;

	mp_get_buffer_raise(st, &bi, MP_BUFFER_RW);
	o = bi.buf;
	if (bi.len < sizeof(*o) || ((uintptr_t)bi.buf & 3U) != 0U || o->magic != PCAP_MAGIC ||
	    o->size != bi.len) {
		mp_raise_ValueError(MP_ERROR_TEXT("not a pcap state"));
	}
	return o;
}

/*
 * The packet ring between the engine's decoder thread and pcap_take(): records of a header and
 * the packet's record and comment, 4-byte aligned, none across the end (RING_PAD there: the rest
 * is skipped). 128 KiB holds some 800 packets: the table's slowest redraws (half a second and
 * more on the S3 with packets coming) leave the decoders room (32 KiB made them wait 855 times
 * in 20 s, the pool filling meanwhile).
 */
#define PKT_RING 131072U
#define RING_PAD 0xFFFFFFFFU

struct ring_hdr {
	uint32_t size;
	uint16_t len, comment_len;
	int64_t at;
	int32_t offset_hz;
	uint32_t duration_us;
	int16_t power_x10, floor_x10;
	uint8_t iface, ok;
};

#ifdef ESDR_PCAP_ENGINE
/* The engine's decoder thread is the ring's only producer. */
static uint32_t ring_free(const struct pcap_obj *o)
{
	return o->ring_size - (o->wr - __atomic_load_n(&o->rd, __ATOMIC_ACQUIRE));
}

/* The producer's side (one thread). */
static bool ring_put(struct pcap_obj *o, const struct esdr_pcap_pkt *p)
{
	const uint32_t clen = p->comment != NULL ? (uint32_t)strlen(p->comment) : 0U;
	const uint32_t need = (uint32_t)(sizeof(struct ring_hdr) + p->len + clen + 3U) & ~3U;
	const uint32_t pos = o->wr & (o->ring_size - 1U);
	const uint32_t skip = o->ring_size - pos < need ? o->ring_size - pos : 0U;
	const struct ring_hdr h = {
		.size = need, .len = p->len, .comment_len = (uint16_t)clen, .at = p->at,
		.offset_hz = p->offset_hz, .duration_us = p->duration_us,
		.power_x10 = p->power_x10, .floor_x10 = p->floor_x10, .iface = p->iface,
		.ok = p->ok ? 1U : 0U,
	};
	uint8_t *at = &o->ring[skip != 0U ? 0U : pos];

	if (ring_free(o) < need + skip) {
		return false;
	}
	if (skip != 0U) {
		const uint32_t pad = RING_PAD;

		memcpy(&o->ring[pos], &pad, sizeof(pad));
	}
	memcpy(at, &h, sizeof(h));
	memcpy(at + sizeof(h), p->rec, p->len);
	if (clen != 0U) {
		memcpy(at + sizeof(h) + p->len, p->comment, clen);
	}
	__atomic_store_n(&o->wr, o->wr + skip + need, __ATOMIC_RELEASE);
	return true;
}
#endif

static mp_obj_t pkt_tuple(const struct ring_hdr *h, const uint8_t *rec, const char *comment)
{
	mp_obj_t t[9] = {
		MP_OBJ_NEW_SMALL_INT(h->iface),
		mp_obj_new_int_from_ll(h->at),
		mp_obj_new_bool(h->ok),
		MP_OBJ_NEW_SMALL_INT(h->power_x10),
		MP_OBJ_NEW_SMALL_INT(h->floor_x10),
		mp_obj_new_int(h->offset_hz),
		mp_obj_new_int_from_uint(h->duration_us),
		mp_obj_new_bytes(rec, h->len),
		comment != NULL ? mp_obj_new_str(comment, h->comment_len) : mp_const_none,
	};

	return mp_obj_new_tuple(9, t);
}

/* The consumer's side (MicroPython): up to @p most packets onto the list @p out. */
static uint32_t ring_take(struct pcap_obj *o, mp_obj_t out, uint32_t most)
{
	uint32_t rd = o->rd, got = 0;

	while (got < most && o->ring != NULL) {
		const uint32_t wr = __atomic_load_n(&o->wr, __ATOMIC_ACQUIRE);
		const uint32_t pos = rd & (o->ring_size - 1U);
		const uint8_t *at = &o->ring[pos];
		struct ring_hdr h;

		if (rd == wr) {
			break;
		}
		/* A pad may sit closer to the end than a header is long. */
		memcpy(&h.size, at, sizeof(h.size));
		if (h.size == RING_PAD) {
			rd += o->ring_size - pos;
			continue;
		}
		memcpy(&h, at, sizeof(h));
		mp_obj_list_append(out, pkt_tuple(&h, at + sizeof(h),
						  h.comment_len != 0U ? (const char *)at + sizeof(h) +
									    h.len
								      : NULL));
		rd += h.size;
		__atomic_store_n(&o->rd, rd, __ATOMIC_RELEASE);
		got++;
	}
	__atomic_store_n(&o->rd, rd, __ATOMIC_RELEASE);
	return got;
}

static mp_obj_t esdr_pcap_new_fn(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
	enum { ARG_freq, ARG_rate, ARG_decoders, ARG_ratio, ARG_gain, ARG_shift, ARG_pool };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_freq, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_rate, MP_ARG_INT, {.u_int = 2000000}},
		{MP_QSTR_decoders, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = PCAP_DEC_DEFAULT}},
		{MP_QSTR_ratio, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_gain, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 255}},
		{MP_QSTR_shift, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_pool, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	struct esdr_pcap_cfg cfg = {0};
	struct pcap_obj *o;
	size_t core, size, ring;
	uint8_t *mem;

	mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
	cfg.freq_hz = (uint64_t)mp_obj_get_float(a[ARG_freq].u_obj);
	cfg.rate_hz = (uint32_t)a[ARG_rate].u_int;
	cfg.decoders = (uint32_t)a[ARG_decoders].u_int;
	cfg.ratio = a[ARG_ratio].u_obj != MP_OBJ_NULL ? (float)mp_obj_get_float(a[ARG_ratio].u_obj)
						       : 4.0f;
	cfg.gain = (uint8_t)a[ARG_gain].u_int;
	cfg.shift = (uint8_t)a[ARG_shift].u_int;
	cfg.clock = k_cycle_get_32;
	cfg.pool = (uint32_t)a[ARG_pool].u_int;
	core = esdr_pcap_size(&cfg);
	if (core == 0U || a[ARG_shift].u_int < 0 || a[ARG_shift].u_int > 8 ||
	    a[ARG_pool].u_int < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap: rate 2 or 4 MS/s, shift 0..8, pool 2..128"));
	}
	ring = cfg.pool != 0U ? PKT_RING : 0U;
	size = (sizeof(*o) + 15U) / 16U * 16U + core + ring;
	mem = m_new0(uint8_t, size);
	o = (struct pcap_obj *)mem;
	o->magic = PCAP_MAGIC;
	o->size = size;
	o->pool = cfg.pool;
	o->det_cpu = -1;
	o->dec_cpu = -1;
	o->core = esdr_pcap_init(mem + (sizeof(*o) + 15U) / 16U * 16U, core, &cfg);
	if (o->core == NULL) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap settings"));
	}
	o->ring = ring != 0U ? mem + (sizeof(*o) + 15U) / 16U * 16U + core : NULL;
	o->ring_size = ring;
	return mp_obj_new_bytearray_by_ref(size, mem);
}
MP_DEFINE_CONST_FUN_OBJ_KW(esdr_pcap_new_obj, 1, esdr_pcap_new_fn);

#ifdef ESDR_PCAP_ENGINE
static bool engine_runs(const struct pcap_obj *o);
#endif

/* The detector's state is the engine's thread's while it runs (on the P4 on the other CPU). */
static void not_running(const struct pcap_obj *o)
{
#ifdef ESDR_PCAP_ENGINE
	if (engine_runs(o)) {
		mp_raise_OSError(EBUSY);
	}
#else
	ARG_UNUSED(o);
#endif
}

static mp_obj_t esdr_pcap_tune_fn(size_t n_args, const mp_obj_t *args)
{
	struct pcap_obj *o = pcap_of(args[0]);

	not_running(o);
	esdr_pcap_tune(o->core, (uint64_t)mp_obj_get_float(args[1]),
		       n_args > 2 ? (uint8_t)mp_obj_get_int(args[2]) : 255U);
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcap_tune_obj, 2, 3, esdr_pcap_tune_fn);

/* The packets of the last call, as tuples (the core keeps them until the next call), onto
 * @p out (a new list if MP_OBJ_NULL). */
static mp_obj_t packets(const struct esdr_pcap *c, uint32_t n, mp_obj_t out)
{
	struct esdr_pcap_pkt p;

	if (out == MP_OBJ_NULL) {
		out = mp_obj_new_list(0, NULL);
	}
	for (uint32_t k = 0; k < n && esdr_pcap_next(c, k, &p); k++) {
		const struct ring_hdr h = {
			.len = p.len, .comment_len = p.comment != NULL ? strlen(p.comment) : 0U,
			.at = p.at, .offset_hz = p.offset_hz, .duration_us = p.duration_us,
			.power_x10 = p.power_x10, .floor_x10 = p.floor_x10, .iface = p.iface,
			.ok = p.ok ? 1U : 0U,
		};

		mp_obj_list_append(out, pkt_tuple(&h, p.rec, p.comment));
	}
	return out;
}

static mp_obj_t esdr_pcap_feed_fn(size_t n_args, const mp_obj_t *args)
{
	struct pcap_obj *o = pcap_of(args[0]);
	mp_buffer_info_t bi;
	mp_int_t start = mp_obj_get_int(args[2]), n = mp_obj_get_int(args[3]);
	long long at = mp_obj_get_ll(args[4]);
	uint32_t analyse = n_args > 5 ? (uint32_t)mp_obj_get_int(args[5]) : 1U;
	uint32_t t0, got;

	not_running(o);
	mp_get_buffer_raise(args[1], &bi, MP_BUFFER_READ);
	if (((uintptr_t)bi.buf & 3U) != 0U || start < 0 || n < 0 || at < 0 ||
	    (size_t)(start + n) > bi.len / 4U) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap_feed: samples outside the buffer"));
	}
	t0 = k_cycle_get_32();
	got = esdr_pcap_feed(o->core, (const int16_t *)bi.buf + 2 * start, (uint32_t)n,
			     (int64_t)at, analyse);
	o->busy_cycles += k_cycle_get_32() - t0;
	o->calls++;
	return packets(o->core, got, MP_OBJ_NULL);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcap_feed_obj, 5, 6, esdr_pcap_feed_fn);

/* ---- the engine (esdr_pcap.h's ESDR_PCAP_ENGINE) ---- */

#ifdef ESDR_PCAP_ENGINE

/* Samples a detector call takes at most; its CPU time before it lets MicroPython run (5 ms). */
#define DET_CHUNK 2048U
#define DET_SLICE (sys_clock_hw_cycles_per_sec() / 200U)
#if defined(ESDR_PCAP_ENGINE_STAGE)
/* Above MicroPython (preemptive 0), below USB (cooperative 8): it runs until it sleeps. */
#define DET_PRIO K_PRIO_COOP(10)
/* The stage holds 8 ms. */
#define DET_IDLE K_USEC(300)
#else
/*
 * The P4: preemptible, so that on one CPU the link's cooperative SDIO reader (3) and parser (2),
 * USB (8) and the system work queue take the CPU at once (a cooperative detector would hold the
 * parser off for up to DET_SLICE); above MicroPython (CONFIG_MAIN_THREAD_PRIORITY 10). With two
 * CPUs it shares CPU 1 with the decoders (below it) and the channelizer (esdr_gl.c, the same
 * priority, never in the same app).
 */
#define DET_PRIO K_PRIO_PREEMPT(9)
#if CONFIG_MAIN_THREAD_PRIORITY <= 9
#error "the Pcap engine's detector must be above MicroPython (the main thread)"
#endif
/* A link block (2040 samples) a millisecond at 2 MS/s; the ring holds far more. */
#define DET_IDLE K_MSEC(1)
#endif
/*
 * The decoders at MicroPython's own priority (pcap_start() reads it: 0 on the S3 and the S31, 10
 * on the P4), yielding after every burst: whenever MicroPython waits (its GUI loop sleeps
 * between passes) they have the CPU, and while it has work they take turns a burst at a time.
 * The time slices (CONFIG_TIMESLICE_SIZE) did not do that (on the S31, with the pool full, the
 * decoders kept the CPU for 13 s and the app's loop stopped meanwhile), probably because Zephyr
 * starts a thread's slice afresh at every switch to it (kswap.h): a thread above (the
 * detector, the link's reader every 100 us tick on the P4) that wakes more often than a slice
 * keeps whichever of the two runs on the CPU. apps/pcap gives its decoder thread 6 KiB.
 */
#define DEC_STACK 6144
/* Free ring bytes a decoder call needs: its metadata records alone may take 64 of some 90. */
#define RING_ROOM 8192U
/*
 * Generic analyses a second at most, one at a time as earned (some 15 ms each on the S3, of
 * bursts no decoder claimed, whose records the table's default filter hides): unlimited, the
 * thread made 12 a second, 18 % of CPU 0.
 */
#define ANALYSE_PER_S 4U
#if defined(ESDR_PCAP_ENGINE_LINK) && defined(CONFIG_SMP)
/*
 * The P4 with both CPUs: the engine on CPU 1, away from MicroPython, the link's threads and
 * USB, which stay on CPU 0 (pin-only: a thread nobody pins runs there). In a busy band the
 * detector and the decoders took some 74 % of a single CPU.
 */
#define ENG_CPU 1
BUILD_ASSERT(IS_ENABLED(CONFIG_SCHED_CPU_MASK_PIN_ONLY),
	     "threads nobody pins must stay on CPU 0, away from the engine's");
#else
/* The S3 and the S31: CPU 1 is the radio's. */
#define ENG_CPU 0
#endif

static struct {
	struct k_thread thread, dec_thread;
	struct k_sem go, done, dec_go, dec_done, kick, room;
	atomic_t run;
	bool started, active;
	struct pcap_obj *o;
#if defined(ESDR_PCAP_ENGINE_STAGE)
	struct esp_sdr_stage_reader r;
#else
	struct esdr_remote_rx_reader r;
#endif
} eng;

#if defined(ESDR_PCAP_ENGINE_STAGE)
/* Where the detector reads: the stage. */
#define src_peek    esp_sdr_stage_peek
#define src_advance esp_sdr_stage_advance
#else
/* Where the detector reads: the link's ring. */
#define src_peek    esdr_remote_rx_peek
#define src_advance esdr_remote_rx_advance
#endif

#if defined(ESDR_PCAP_ENGINE_STAGE) && defined(CONFIG_ESP_SDR_RING)
/* In the module's bss: the high region is MicroPython's stack and the radio threads'. */
static K_KERNEL_STACK_DEFINE(det_stack, 3072);
static K_KERNEL_STACK_DEFINE(dec_stack, DEC_STACK);

static int engine_rx_start(uint32_t rate, const mp_buffer_info_t *ring)
{
	int ret = esdr_rx_start_stage(rate == 2000000U);

	ARG_UNUSED(ring);
	if (ret == 0) {
		esp_sdr_stage_reader_init(&eng.r);
	}
	return ret;
}

static void engine_rx_stop(void)
{
	esdr_rx_stop();
	(void)esdr_rx_wait(-1);
}
#else
/*
 * In PSRAM: the S31's internal RAM is full, the P4's 94 % (mp-pcap: 370000 of 384 KiB). Neither
 * thread runs while the cache is off: a flash write holds interrupts off meanwhile
 * (spi_flash_disable_interrupts_caches_and_other_cpu()).
 */
static Z_KERNEL_STACK_DEFINE_IN(det_stack, 3072, ESP_SDR_DATA_F(STACK, det_stack, NOINIT));
static Z_KERNEL_STACK_DEFINE_IN(dec_stack, DEC_STACK, ESP_SDR_DATA_F(STACK, dec_stack, NOINIT));

#if defined(ESDR_PCAP_ENGINE_STAGE)
static int engine_rx_start(uint32_t rate, const mp_buffer_info_t *ring)
{
	int ret = esdr_remote_rx_start_stage(rate);

	ARG_UNUSED(ring);
	if (ret == 0) {
		esp_sdr_stage_reader_init(&eng.r);
	}
	return ret;
}
#else
/* The link's stream into the ring (as esdr.rx_start(rate, ring, wrap=True)), its reader at the
 * run's start. */
static int engine_rx_start(uint32_t rate, const mp_buffer_info_t *ring)
{
	int ret = esdr_remote_init(NULL);

	if (ret == 0) {
		ret = esdr_remote_rx_start(rate, ring->buf, ring->len / 4U, 0, true);
	}
	if (ret == 0) {
		ret = esdr_remote_rx_reader_init(&eng.r);
		if (ret != 0) {
			esdr_remote_rx_stop();
		}
	}
	return ret;
}
#endif

static void engine_rx_stop(void)
{
	esdr_remote_rx_stop();
}
#endif

static bool engine_runs(const struct pcap_obj *o)
{
	return eng.active && eng.o == o;
}

static void det_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		struct pcap_obj *o;
		uint32_t spent = 0;

		(void)k_sem_take(&eng.go, K_FOREVER);
		o = eng.o;
		o->det_cpu = (int8_t)arch_curr_cpu()->id;
		while (atomic_get(&eng.run)) {
			const uint32_t *w;
			uint64_t index;
			uint32_t lapped = eng.r.lapped;
			uint32_t n = src_peek(&eng.r, &w, &index, DET_CHUNK);
			uint32_t t0, d;

			o->lapped += eng.r.lapped - lapped;
			if (n == 0U) {
				spent = 0;
				k_sleep(DET_IDLE);
				continue;
			}
			t0 = k_cycle_get_32();
			/* A word is an int16 I, Q pair in memory. */
			esdr_pcap_detect(o->core, (const int16_t *)w, n, (int64_t)index);
			if (!src_advance(&eng.r, n)) {
				o->bad += n;
			}
			if (esdr_pcap_pending(o->core) != 0U) {
				k_sem_give(&eng.kick);
			}
			d = k_cycle_get_32() - t0;
			o->det_cycles += d;
			spent += d;
			if (spent >= DET_SLICE) {
				/* Behind: the stage holds 8 ms (the P4's ring 65 ms at least, pcap.py's
				 * 130); the display and the knob (on CPU 1 the decoders) get a turn. */
				spent = 0;
				k_sleep(K_TICKS(1));
			}
		}
		k_sem_give(&eng.done);
	}
}

/*
 * The closed bursts decoded a burst a call, the packets into the ring. The generic analysis
 * only while the queue is under a quarter of the pool, ANALYSE_PER_S at most (apps/pcap's
 * decoder.c gives it to separate workers).
 */
static void dec_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		struct pcap_obj *o;
		/* The analyses earned (milliseconds' worth) and when last counted. */
		int64_t earned = 0, last = k_uptime_get();

		(void)k_sem_take(&eng.dec_go, K_FOREVER);
		o = eng.o;
		o->dec_cpu = (int8_t)arch_curr_cpu()->id;
		while (atomic_get(&eng.run)) {
			const uint32_t pending = esdr_pcap_pending(o->core);
			struct esdr_pcap_pkt p;
			uint32_t t0, n, analyse;
			bool analysed;
			const int64_t now = k_uptime_get();

			if (pending == 0U) {
				(void)k_sem_take(&eng.kick, K_MSEC(10));
				continue;
			}
			if (ring_free(o) < RING_ROOM) {
				/* MicroPython takes them (pcap_take()): until then the pool fills. */
				o->ring_waits++;
				(void)k_sem_take(&eng.room, K_MSEC(10));
				continue;
			}
			earned = MIN(earned + (now - last) * ANALYSE_PER_S, 1000);
			last = now;
			analyse = 4U * pending < o->pool && earned >= 1000 ? 1U : 0U;
			t0 = k_cycle_get_32();
			n = esdr_pcap_decode(o->core, 1U, analyse);
			analysed = analyse != 0U && esdr_pcap_analyses_left(o->core) == 0U;
			if (analysed) {
				earned -= 1000;
			}
			for (uint32_t k = 0; k < n && esdr_pcap_next(o->core, k, &p); k++) {
				/*
				 * With the ring half full (MicroPython takes them slower than they
				 * come), the records of bursts' numbers alone stay out (not the
				 * generic analysis's, paid for already): in a busy band most records
				 * are these (the Wi-Fi's wideband bursts), and a full ring stops the
				 * decoders, the pool fills and packets go.
				 */
				if (p.iface == PCAP_IF_BURST && !analysed &&
				    ring_free(o) < o->ring_size / 2U) {
					o->shed++;
				} else if (!ring_put(o, &p)) {
					o->ring_lost++;
				}
			}
			o->dec_cycles += k_cycle_get_32() - t0;
			k_yield();
		}
		k_sem_give(&eng.dec_done);
	}
}

static void engine_halt(void)
{
	if (!eng.active) {
		return;
	}
	atomic_set(&eng.run, 0);
	k_sem_give(&eng.kick);
	k_sem_give(&eng.room);
	(void)k_sem_take(&eng.done, K_FOREVER);
	(void)k_sem_take(&eng.dec_done, K_FOREVER);
	eng.active = false;
	/* No thread reads the ring now; once the parser is out of it, it may go. */
	engine_rx_stop();
	MP_STATE_PORT(esdr_pcap_running) = MP_OBJ_NULL;
#ifdef ESDR_PCAP_ENGINE_LINK
	MP_STATE_PORT(esdr_pcap_ring) = MP_OBJ_NULL;
#endif
}

void esdr_pcap_deinit(void)
{
	/* Before the heap goes: both threads use the state in it. */
	engine_halt();
}

static void thread_up(struct k_thread *t, k_thread_stack_t *stack, size_t size,
		      k_thread_entry_t fn, int prio, const char *name)
{
	k_thread_create(t, stack, size, fn, NULL, NULL, NULL, prio, K_FP_REGS, K_FOREVER);
	k_thread_name_set(t, name);
#if defined(CONFIG_SCHED_CPU_MASK)
	if (arch_num_cpus() > 1) {
		/* Not runnable yet (K_FOREVER): the pin takes. */
		int ret = k_thread_cpu_pin(t, ENG_CPU);

		__ASSERT(ret == 0, "pin to CPU %d: %d", ENG_CPU, ret);
		ARG_UNUSED(ret);
	}
#endif
	k_thread_start(t);
}

static mp_obj_t esdr_pcap_start_fn(size_t n_args, const mp_obj_t *args)
{
	mp_obj_t st = args[0];
	struct pcap_obj *o = pcap_of(st);
	uint32_t rate = esdr_pcap_rate(o->core);
	mp_obj_t ring_obj = n_args > 1 ? args[1] : mp_const_none;
	mp_buffer_info_t ring = {0};
	int ret;

	if (eng.active) {
		mp_raise_OSError(EBUSY);
	}
	if (rate != 2000000U && rate != 4000000U) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap_start: 2 or 4 MS/s"));
	}
	if (o->ring == NULL) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap_start: a state with a pool"));
	}
#ifdef ESDR_PCAP_ENGINE_LINK
	if (ring_obj == mp_const_none) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap_start: a ring of PCAP_RING_MIN samples"));
	}
	mp_get_buffer_raise(ring_obj, &ring, MP_BUFFER_WRITE);
	if (((uintptr_t)ring.buf & 3U) != 0U || ring.len / 4U < ESDR_PCAP_RING_MIN ||
	    ring.len / 4U > INT32_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcap_start: a ring of PCAP_RING_MIN samples"));
	}
#else
	ARG_UNUSED(ring_obj);
#endif
	if (!eng.started) {
		k_sem_init(&eng.go, 0, 1);
		k_sem_init(&eng.done, 0, 1);
		k_sem_init(&eng.dec_go, 0, 1);
		k_sem_init(&eng.dec_done, 0, 1);
		k_sem_init(&eng.kick, 0, 1);
		k_sem_init(&eng.room, 0, 1);
		thread_up(&eng.thread, det_stack, K_KERNEL_STACK_SIZEOF(det_stack), det_fn, DET_PRIO,
			  "esdr_pcap");
		/* MicroPython's own priority (this is its thread). */
		thread_up(&eng.dec_thread, dec_stack, K_KERNEL_STACK_SIZEOF(dec_stack), dec_fn,
			  k_thread_priority_get(k_current_get()), "esdr_pcap_dec");
		eng.started = true;
	}
#ifdef ESDR_PCAP_ENGINE_LINK
	/* Rooted before the parser writes into it. */
	MP_STATE_PORT(esdr_pcap_ring) = ring_obj;
#endif
	ret = engine_rx_start(rate, &ring);
	if (ret != 0) {
#ifdef ESDR_PCAP_ENGINE_LINK
		MP_STATE_PORT(esdr_pcap_ring) = MP_OBJ_NULL;
#endif
		mp_raise_OSError(-ret);
	}
	MP_STATE_PORT(esdr_pcap_running) = st;
	eng.o = o;
	eng.active = true;
	k_sem_reset(&eng.kick);
	k_sem_reset(&eng.room);
	atomic_set(&eng.run, 1);
	k_sem_give(&eng.go);
	k_sem_give(&eng.dec_go);
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcap_start_obj, 1, 2, esdr_pcap_start_fn);

static mp_obj_t esdr_pcap_stop_fn(mp_obj_t st)
{
	struct pcap_obj *o = pcap_of(st);

	if (engine_runs(o)) {
		engine_halt();
		esdr_pcap_end(o->core);
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_pcap_stop_obj, esdr_pcap_stop_fn);

#else
static bool engine_runs(const struct pcap_obj *o)
{
	ARG_UNUSED(o);
	return false;
}
#endif /* ESDR_PCAP_ENGINE */

static mp_obj_t esdr_pcap_flush_fn(size_t n_args, const mp_obj_t *args)
{
	struct pcap_obj *o = pcap_of(args[0]);
	mp_obj_t out = mp_obj_new_list(0, NULL);
	uint32_t got;

	if (engine_runs(o)) {
		mp_raise_OSError(EBUSY);
	}
	/* What the engine's decoders left in the ring first: they decoded it earlier. */
	(void)ring_take(o, out, UINT32_MAX);
	got = esdr_pcap_flush(o->core, n_args > 1 ? (uint32_t)mp_obj_get_int(args[1]) : 1U);
	return packets(o->core, got, out);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcap_flush_obj, 1, 2, esdr_pcap_flush_fn);

static mp_obj_t esdr_pcap_take_fn(size_t n_args, const mp_obj_t *args)
{
	struct pcap_obj *o = pcap_of(args[0]);
	uint32_t most = n_args > 1 ? (uint32_t)mp_obj_get_int(args[1]) : 64U;
	uint32_t analyse = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : 2U;
	uint32_t t0 = k_cycle_get_32(), got;
	mp_obj_t out;

	if (engine_runs(o)) {
		/* The decoder thread's packets. */
		out = mp_obj_new_list(0, NULL);
		(void)ring_take(o, out, most);
#ifdef ESDR_PCAP_ENGINE
		k_sem_give(&eng.room);
#endif
		o->busy_cycles += k_cycle_get_32() - t0;
		o->calls++;
		return out;
	}
	got = esdr_pcap_decode(o->core, most, analyse);
	o->busy_cycles += k_cycle_get_32() - t0;
	o->calls++;
	return packets(o->core, got, MP_OBJ_NULL);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcap_take_obj, 1, 3, esdr_pcap_take_fn);

static mp_obj_t esdr_pcap_pending_fn(mp_obj_t st)
{
	return mp_obj_new_int_from_uint(esdr_pcap_pending(pcap_of(st)->core));
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_pcap_pending_obj, esdr_pcap_pending_fn);

static uint64_t cycles_us(uint64_t cycles)
{
	return cycles * 1000000U / sys_clock_hw_cycles_per_sec();
}

static void stat(mp_obj_t d, qstr key, mp_obj_t v)
{
	mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), v);
}

static mp_obj_t esdr_pcap_stats_fn(mp_obj_t st)
{
	struct pcap_obj *o = pcap_of(st);
	struct esdr_pcap_stats s;
	mp_obj_t d = mp_obj_new_dict(31);

	esdr_pcap_get_stats(o->core, &s);
	stat(d, MP_QSTR_samples, mp_obj_new_int_from_ull(s.samples));
	stat(d, MP_QSTR_gaps, mp_obj_new_int_from_uint(s.gaps));
	stat(d, MP_QSTR_bursts, mp_obj_new_int_from_uint(s.bursts));
	stat(d, MP_QSTR_blips, mp_obj_new_int_from_uint(s.blips));
	stat(d, MP_QSTR_truncated, mp_obj_new_int_from_uint(s.truncated));
	stat(d, MP_QSTR_ble, mp_obj_new_int_from_uint(s.ble));
	stat(d, MP_QSTR_ble_bad, mp_obj_new_int_from_uint(s.ble_bad));
	stat(d, MP_QSTR_wpan, mp_obj_new_int_from_uint(s.wpan));
	stat(d, MP_QSTR_wpan_bad, mp_obj_new_int_from_uint(s.wpan_bad));
	stat(d, MP_QSTR_generic, mp_obj_new_int_from_uint(s.generic));
	stat(d, MP_QSTR_wide, mp_obj_new_int_from_uint(s.wide));
	stat(d, MP_QSTR_skipped, mp_obj_new_int_from_uint(s.skipped));
	stat(d, MP_QSTR_lost, mp_obj_new_int_from_uint(s.lost));
	stat(d, MP_QSTR_calls, mp_obj_new_int_from_uint(o->calls));
	stat(d, MP_QSTR_busy_us, mp_obj_new_int_from_ull(cycles_us(o->busy_cycles)));
	stat(d, MP_QSTR_ble_us, mp_obj_new_int_from_ull(cycles_us(s.cyc_ble)));
	stat(d, MP_QSTR_wpan_us, mp_obj_new_int_from_ull(cycles_us(s.cyc_wpan)));
	stat(d, MP_QSTR_generic_us, mp_obj_new_int_from_ull(cycles_us(s.cyc_generic)));
	stat(d, MP_QSTR_det_us, mp_obj_new_int_from_ull(cycles_us(o->det_cycles)));
	stat(d, MP_QSTR_pool_full, mp_obj_new_int_from_uint(s.pool_full));
	stat(d, MP_QSTR_lapped, mp_obj_new_int_from_uint(o->lapped));
	stat(d, MP_QSTR_bad, mp_obj_new_int_from_uint(o->bad));
	stat(d, MP_QSTR_pending, mp_obj_new_int_from_uint(esdr_pcap_pending(o->core)));
	stat(d, MP_QSTR_dec_us, mp_obj_new_int_from_ull(cycles_us(o->dec_cycles)));
	stat(d, MP_QSTR_ring_lost, mp_obj_new_int_from_uint(o->ring_lost));
	stat(d, MP_QSTR_ring_waits, mp_obj_new_int_from_uint(o->ring_waits));
	stat(d, MP_QSTR_shed, mp_obj_new_int_from_uint(o->shed));
	stat(d, MP_QSTR_det_cpu, MP_OBJ_NEW_SMALL_INT(o->det_cpu));
	stat(d, MP_QSTR_dec_cpu, MP_OBJ_NEW_SMALL_INT(o->dec_cpu));
	return d;
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_pcap_stats_obj, esdr_pcap_stats_fn);

/* ---- the table's fields (esdr_pcap_dissect.c: pcap_view.py's dissector in C) ---- */

/* A str of the text, without mp_obj_new_str()'s look for an equal qstr (it walks every pool). */
static mp_obj_t text(const struct esdr_pcap_text *t)
{
	if (t->len == 0U) {
		return MP_OBJ_NEW_QSTR(MP_QSTR_);
	}
	return mp_obj_new_str_copy(&mp_type_str, (const byte *)t->s, t->len);
}

/*
 * The dissector's fixed words (types, PHYs, channels) as str objects in flash: no allocation for
 * them, and the packets kept share them. Hash 0: computed when asked.
 */
#define ROM_STR(s) {{&mp_type_str}, 0, sizeof(s) - 1U, (const byte *)(s)}
static const mp_obj_str_t words[] = {
	ROM_STR("ADV_IND"),     ROM_STR("ADV_DIRECT"),    ROM_STR("ADV_NONCONN"),
	ROM_STR("SCAN_REQ"),    ROM_STR("SCAN_RSP"),      ROM_STR("CONNECT_IND"),
	ROM_STR("ADV_SCAN"),    ROM_STR("ADV_EXT"),       ROM_STR("AUX_CONN_RSP"),
	ROM_STR("LL_CONN_UPDATE"), ROM_STR("LL_CHAN_MAP"), ROM_STR("LL_TERMINATE"),
	ROM_STR("LL_ENC_REQ"),  ROM_STR("LL_ENC_RSP"),    ROM_STR("LL_VERSION"),
	ROM_STR("LL_LENGTH_REQ"), ROM_STR("LL_LENGTH_RSP"), ROM_STR("Empty"),
	ROM_STR("L2CAP"),       ROM_STR("L2CAP+"),        ROM_STR("Beacon"),
	ROM_STR("Data"),        ROM_STR("Ack"),           ROM_STR("Cmd"),
	ROM_STR("Rsvd"),        ROM_STR("Multi"),         ROM_STR("Frag"),
	ROM_STR("Ext"),         ROM_STR("AssocReq"),      ROM_STR("AssocRsp"),
	ROM_STR("Disassoc"),    ROM_STR("DataReq"),       ROM_STR("PanConflict"),
	ROM_STR("Orphan"),      ROM_STR("BeaconReq"),     ROM_STR("Realign"),
	ROM_STR("GtsReq"),      ROM_STR("?"),             ROM_STR("CW"),
	ROM_STR("OOK"),         ROM_STR("2FSK"),          ROM_STR("4FSK"),
	ROM_STR("MSK"),         ROM_STR("BPSK"),          ROM_STR("QPSK"),
	ROM_STR("Wide"),        ROM_STR("LE 1M"),         ROM_STR("LE 2M"),
	ROM_STR("LE Coded"),    ROM_STR("O-QPSK"),        ROM_STR("malformed"),
};
/* "0" to "39": the Bluetooth LE channels, 802.15.4's 11 to 26 among them. */
static const mp_obj_str_t channels[] = {
	ROM_STR("0"),  ROM_STR("1"),  ROM_STR("2"),  ROM_STR("3"),  ROM_STR("4"),  ROM_STR("5"),
	ROM_STR("6"),  ROM_STR("7"),  ROM_STR("8"),  ROM_STR("9"),  ROM_STR("10"), ROM_STR("11"),
	ROM_STR("12"), ROM_STR("13"), ROM_STR("14"), ROM_STR("15"), ROM_STR("16"), ROM_STR("17"),
	ROM_STR("18"), ROM_STR("19"), ROM_STR("20"), ROM_STR("21"), ROM_STR("22"), ROM_STR("23"),
	ROM_STR("24"), ROM_STR("25"), ROM_STR("26"), ROM_STR("27"), ROM_STR("28"), ROM_STR("29"),
	ROM_STR("30"), ROM_STR("31"), ROM_STR("32"), ROM_STR("33"), ROM_STR("34"), ROM_STR("35"),
	ROM_STR("36"), ROM_STR("37"), ROM_STR("38"), ROM_STR("39"),
};

static mp_obj_t word(const struct esdr_pcap_text *t)
{
	for (size_t k = 0; k < MP_ARRAY_SIZE(words); k++) {
		if (words[k].len == t->len && memcmp(words[k].data, t->s, t->len) == 0) {
			return MP_OBJ_FROM_PTR(&words[k]);
		}
	}
	return text(t);
}

static mp_obj_t channel(const struct esdr_pcap_text *t)
{
	uint32_t v = 0;

	if (t->len == 0U || t->len > 2U || (t->len == 2U && t->s[0] == '0')) {
		return text(t);
	}
	for (size_t k = 0; k < t->len; k++) {
		if (t->s[k] < '0' || t->s[k] > '9') {
			return text(t);
		}
		v = v * 10U + (uint32_t)(t->s[k] - '0');
	}
	return v < MP_ARRAY_SIZE(channels) ? MP_OBJ_FROM_PTR(&channels[v]) : text(t);
}

static mp_obj_t esdr_pcap_dissect_fn(mp_obj_t iface, mp_obj_t rec)
{
	mp_buffer_info_t bi;
	struct esdr_pcap_fields f;

	mp_get_buffer_raise(rec, &bi, MP_BUFFER_READ);
	esdr_pcap_dissect((uint32_t)mp_obj_get_int(iface), bi.buf, bi.len, &f);
	mp_obj_t t[9] = {
		channel(&f.channel), word(&f.phy), word(&f.kind), text(&f.src), text(&f.dst),
		mp_obj_new_bool(f.random), text(&f.info), mp_obj_new_int_from_uint(f.length),
		f.ok < 0 ? mp_const_none : mp_obj_new_bool(f.ok),
	};

	return mp_obj_new_tuple(9, t);
}
MP_DEFINE_CONST_FUN_OBJ_2(esdr_pcap_dissect_obj, esdr_pcap_dissect_fn);

#ifdef ESDR_PCAP_ENGINE
/*
 * pcap_take()'s packets as the egg's Pkt objects (eggs/pcap/pcap_view.py: what its __init__()
 * and _dissect() set, neither called), dissected here, their attribute map sized once for all
 * PKT_FIELDS: Python's stores grew it eight times, each a new table in a fragmented heap that
 * the allocator searched from the last freed block (on the XIAO 135 us a packet for the first
 * twelve, 600 more for the dissector's eight).
 */
#define PKT_FIELDS 20U
#define PKT_SLOTS  23U

static void pkt_put(mp_obj_instance_t *o, qstr q, mp_obj_t v)
{
	mp_map_lookup(&o->members, MP_OBJ_NEW_QSTR(q), MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = v;
}

/*
 * pcap_take_pkts(state, n, cls, no, t0, at0, rate, freq) -> [Pkt]: up to n packets the
 * decoders made, numbered from no + 1, at t0 + (at - at0) / rate seconds, freq their channel's.
 */
static mp_obj_t esdr_pcap_take_pkts_fn(size_t n_args, const mp_obj_t *a)
{
	struct pcap_obj *o = pcap_of(a[0]);
	const uint32_t most = (uint32_t)mp_obj_get_int(a[1]);
	mp_int_t no = mp_obj_get_int(a[3]);
	const mp_float_t t0 = mp_obj_get_float(a[4]);
	const long long at0 = mp_obj_get_ll(a[5]);
	const mp_float_t rate = mp_obj_get_float(a[6]);
	const uint32_t c0 = k_cycle_get_32();
	uint32_t rd = o->rd, got = 0;
	mp_obj_t out;

	ARG_UNUSED(n_args);
	/* A class of the egg's (not a native type, whose instances look otherwise). */
	if (!mp_obj_is_type(a[2], &mp_type_type) ||
	    ((mp_obj_type_t *)MP_OBJ_TO_PTR(a[2]))->name != MP_QSTR_Pkt) {
		mp_raise_TypeError(MP_ERROR_TEXT("pcap_take_pkts: pcap_view.Pkt"));
	}
	if (!engine_runs(o)) {
		mp_raise_OSError(EINVAL);
	}
	out = mp_obj_new_list(0, NULL);
	while (got < most && o->ring != NULL) {
		const uint32_t wr = __atomic_load_n(&o->wr, __ATOMIC_ACQUIRE);
		const uint32_t pos = rd & (o->ring_size - 1U);
		const uint8_t *at = &o->ring[pos];
		const mp_obj_type_t *native_base = NULL;
		struct esdr_pcap_fields f;
		mp_obj_instance_t *p;
		struct ring_hdr h;

		if (rd == wr) {
			break;
		}
		memcpy(&h.size, at, sizeof(h.size));
		if (h.size == RING_PAD) {
			rd += o->ring_size - pos;
			continue;
		}
		memcpy(&h, at, sizeof(h));
		esdr_pcap_dissect(h.iface, at + sizeof(h), h.len, &f);
		p = mp_obj_new_instance(MP_OBJ_TO_PTR(a[2]), &native_base);
		mp_map_init(&p->members, PKT_SLOTS);
		pkt_put(p, MP_QSTR_iface, MP_OBJ_NEW_SMALL_INT(h.iface));
		pkt_put(p, MP_QSTR_at, mp_obj_new_int_from_ll(h.at));
		pkt_put(p, MP_QSTR_ok, mp_obj_new_bool(f.ok >= 0 ? f.ok != 0 : h.ok != 0U));
		pkt_put(p, MP_QSTR_offset, mp_obj_new_int(h.offset_hz));
		pkt_put(p, MP_QSTR_us, mp_obj_new_int_from_uint(h.duration_us));
		pkt_put(p, MP_QSTR_rec, mp_obj_new_bytes(at + sizeof(h), h.len));
		pkt_put(p, MP_QSTR_comment,
			h.comment_len != 0U
				? mp_obj_new_str_copy(&mp_type_str,
						      at + sizeof(h) + h.len, h.comment_len)
				: mp_const_none);
		pkt_put(p, MP_QSTR_no, mp_obj_new_int(++no));
		pkt_put(p, MP_QSTR_t, mp_obj_new_float(t0 + (mp_float_t)(h.at - at0) / rate));
		pkt_put(p, MP_QSTR_freq, a[7]);
		pkt_put(p, MP_QSTR_power, mp_obj_new_float((mp_float_t)h.power_x10 / 10));
		pkt_put(p, MP_QSTR_floor, mp_obj_new_float((mp_float_t)h.floor_x10 / 10));
		pkt_put(p, MP_QSTR_channel, channel(&f.channel));
		pkt_put(p, MP_QSTR_phy, word(&f.phy));
		pkt_put(p, MP_QSTR_kind, word(&f.kind));
		pkt_put(p, MP_QSTR_src, text(&f.src));
		pkt_put(p, MP_QSTR_dst, text(&f.dst));
		pkt_put(p, MP_QSTR_random, mp_obj_new_bool(f.random));
		pkt_put(p, MP_QSTR_info, text(&f.info));
		pkt_put(p, MP_QSTR_length, mp_obj_new_int_from_uint(f.length));
		mp_obj_list_append(out, MP_OBJ_FROM_PTR(p));
		rd += h.size;
		__atomic_store_n(&o->rd, rd, __ATOMIC_RELEASE);
		got++;
	}
	k_sem_give(&eng.room);
	o->busy_cycles += k_cycle_get_32() - c0;
	o->calls++;
	return out;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcap_take_pkts_obj, 8, 8, esdr_pcap_take_pkts_fn);
#endif

/* ---- pcapng (esdr_pcap_core.c: apps/pcap's writer and interfaces) ---- */

static mp_obj_t esdr_pcapng_head_fn(mp_obj_t hw, mp_obj_t os, mp_obj_t appl)
{
	vstr_t v;
	int n;

	vstr_init(&v, 1024);
	n = esdr_pcap_head((uint8_t *)v.buf, v.alloc, mp_obj_str_get_str(hw),
			   mp_obj_str_get_str(os), mp_obj_str_get_str(appl));
	if (n < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcapng: names too long"));
	}
	v.len = (size_t)n;
	return mp_obj_new_bytes_from_vstr(&v);
}
MP_DEFINE_CONST_FUN_OBJ_3(esdr_pcapng_head_obj, esdr_pcapng_head_fn);

static mp_obj_t esdr_pcapng_epb_fn(size_t n_args, const mp_obj_t *args)
{
	mp_buffer_info_t bi;
	const char *comment = NULL;
	uint32_t iface = (uint32_t)mp_obj_get_int(args[0]);
	uint64_t t_ns = (uint64_t)mp_obj_get_ll(args[1]);
	vstr_t v;
	int n;

	mp_get_buffer_raise(args[2], &bi, MP_BUFFER_READ);
	if (n_args > 3 && args[3] != mp_const_none) {
		comment = mp_obj_str_get_str(args[3]);
	}
	if (iface >= PCAP_IF_COUNT || bi.len > UINT16_MAX) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcapng_epb: interface or length"));
	}
	vstr_init(&v, esdr_pcap_epb_size((uint32_t)bi.len, comment));
	n = esdr_pcap_epb((uint8_t *)v.buf, v.alloc, iface, t_ns, bi.buf, (uint32_t)bi.len, comment);
	if (n < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("pcapng_epb"));
	}
	v.len = (size_t)n;
	return mp_obj_new_bytes_from_vstr(&v);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_pcapng_epb_obj, 3, 4, esdr_pcapng_epb_fn);
