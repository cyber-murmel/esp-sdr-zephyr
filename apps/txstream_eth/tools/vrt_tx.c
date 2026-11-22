// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vrt_tx: send a sample stream to the ESP32-S31's transmit stream (apps/txstream_eth) as VITA 49.2
 * signal data packets over UDP, paced to the sample rate by the board's own status.
 *
 *   vrt_tx --to <addr> --rate 20|40 --fmt iq8|w32 --source tone:kHz[:amp]|file:PATH|stdin|board
 *          [--seconds N] [--rf] [--verify] [--loop] [--iface NAME] [--port 4992] [--dport 4994]
 *          [--max-amp 4] [--allow-amp] [--gso] [--rt] [--spin-us 30] [--pkt-words N] [--max-mbps 900] [-q]
 *
 * Protocol: doc/dev/txs.md "Network protocol", apps/txstream_eth/src/tx_proto.h. The cookie
 * handshake of the control port is the one of vrt_udp (VRTS, VRTC + cookie, VRTK), then TXGO
 * (answered by TXAK), TXST records from the board every 250 us, TXEN to drain. Control and data go
 * from one socket (the board's hook filter accepts data from the subscriber's address and port).
 * IPv6 first, like vrt_udp: link-local with --iface.
 *
 * Threads: the main thread is the control thread (poll, handshake, renewals every 250 ms, pings
 * every 10 ms, TXST into the estimator, a line a second); the sender thread plans with
 * tx_pace.h (under one mutex), fills 6 packets and sends them with one sendmmsg (or one GSO
 * sendmsg), then sleeps until the estimated lead falls to its wake level, the last --spin-us of it
 * spinning. SCHED_FIFO with --rt (needs the privilege; else plain).
 *
 * --source board: the board's own test tone (k = 18 per 351 samples, 4 DAC counts, tx_tone_gen.h), a function of
 * the stream index; with --verify the board reads its bank back against it and `tx net` shows the wrong words.
 *
 * Amplitude: iq8 counts, 1 count = 4 DAC counts of 511. Default 1; above --max-amp (4) refused
 * unless --allow-amp. Files are scanned first. The board does not clip.
 *
 * Exit: 0 clean (no underrun, loss, late word, resync), 1 not clean or no contact, 2 usage,
 * 3 another host has the stream or the board is busy, 4 the board refused the run.
 *
 * Build: make -C apps/txstream_eth/tools   (gcc, no other library)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "../src/tx_proto.h"
#include "tx_core.h"
#include "tx_src.h"
#include "../src/tx_tone_gen.h"

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef SOL_UDP
#define SOL_UDP 17
#endif

#define HDR_BYTES   16U
#define MAX_BATCH   16U
#define PKT_MAX     1500U


static int sock = -1;
static struct sockaddr_storage ctl_addr, data_addr;
static socklen_t addr_len;
static _Atomic bool stop_req, sender_done, src_eof, run_ok;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv; /* signalled on a status (CLOCK_MONOTONIC), for the sender waiting on its credit */
static struct txc core; /* the session and pacing core (tx_core.h), under mu */
static bool quiet;
static int64_t arm_wait_ns;

static uint32_t rate_msps = 20, fmt = TXP_FMT_IQ8, pkt_words, go_flags;
static double seconds;
static bool loop_file, use_gso, use_rt;
static int64_t spin_ns = 30000;
static uint32_t max_mbps = TXP_MAX_MBPS;

static _Atomic uint64_t sent_pkts, sent_words, send_err, starved;

/* the source */
enum { SRC_TONE, SRC_FILE, SRC_STDIN, SRC_BOARD };
static int src_kind;
static struct txsrc_tone tone;
static uint8_t *file_buf;
static size_t file_words;
static uint8_t *ring;
static size_t ring_size = 16U << 20;
static _Atomic size_t ring_head, ring_tail;
static uint32_t sample_bytes;

static int64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void eprint(const char *fmt_, ...) __attribute__((format(printf, 1, 2)));
static void eprint(const char *fmt_, ...)
{
	va_list ap;

	va_start(ap, fmt_);
	vfprintf(stderr, fmt_, ap);
	va_end(ap);
}

static void on_sig(int s)
{
	(void)s;
	atomic_store(&stop_req, true);
}

static void usage(void)
{
	eprint("usage: vrt_tx --to <addr> --rate 20|40 --fmt iq8|w32 --source tone:kHz[:amp]|file:PATH|stdin|board\n"
	       "              [--seconds N] [--rf] [--verify] [--loop] [--iface NAME] [--port 4992]\n"
	       "              [--dport 4994] [--max-amp 4] [--allow-amp] [--gso] [--rt] [--spin-us 30]\n"
	       "              [--pkt-words N] [--max-mbps 900] [--arm-wait 60] [-q]\n");
}

/* ---- the sources ---- */

static void *stdin_reader(void *arg)
{
	(void)arg;
	for (;;) {
		size_t h = atomic_load_explicit(&ring_head, memory_order_relaxed);
		size_t t = atomic_load_explicit(&ring_tail, memory_order_acquire);
		size_t room = ring_size - (h - t);
		size_t off = h % ring_size, n = room < ring_size - off ? room : ring_size - off;
		ssize_t r;

		if (atomic_load(&stop_req)) {
			return NULL;
		}
		if (n == 0) {
			nanosleep(&(struct timespec){0, 500000}, NULL);
			continue;
		}
		r = read(0, ring + off, n);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			atomic_store(&src_eof, true);
			return NULL;
		}
		atomic_store_explicit(&ring_head, h + (size_t)r, memory_order_release);
	}
}

/* words samples from stream index idx (from the start of the run: idx - base) into dst */
static void source_fill(uint64_t idx, uint64_t base, uint32_t words, uint8_t *dst)
{
	size_t bytes = (size_t)words * sample_bytes;

	switch (src_kind) {
	case SRC_TONE:
		txsrc_tone_fill(&tone, idx, words, dst, fmt);
		break;
	case SRC_BOARD:
		/* the board's own tone (tx_tone_gen.h): what its --verify reads the bank back against */
		for (uint32_t k = 0; k < words; k++) {
			if (fmt == TXP_FMT_IQ8) {
				int8_t i8, q8;

				txtg_iq8(idx + k, &i8, &q8);
				dst[2U * k] = (uint8_t)i8;
				dst[2U * k + 1U] = (uint8_t)q8;
			} else {
				const uint32_t w = txtg_word(idx + k);

				dst[4U * k] = (uint8_t)w;
				dst[4U * k + 1U] = (uint8_t)(w >> 8);
				dst[4U * k + 2U] = (uint8_t)(w >> 16);
				dst[4U * k + 3U] = (uint8_t)(w >> 24);
			}
		}
		break;
	case SRC_FILE: {
		uint64_t pos = idx - base;
		size_t got = 0;

		while (got < words) {
			uint64_t p = loop_file ? pos % file_words : pos;
			size_t n;

			if (p >= file_words) {
				break;
			}
			n = file_words - p < words - got ? file_words - p : words - got;
			memcpy(dst + got * sample_bytes, file_buf + p * sample_bytes, n * sample_bytes);
			got += n;
			pos += n;
		}
		memset(dst + got * sample_bytes, 0, bytes - got * sample_bytes);
		break;
	}
	default: {
		size_t t = atomic_load_explicit(&ring_tail, memory_order_relaxed);
		size_t h = atomic_load_explicit(&ring_head, memory_order_acquire);
		size_t have = (h - t) / sample_bytes * sample_bytes, n = have < bytes ? have : bytes;

		for (size_t k = 0; k < n; k++) {
			dst[k] = ring[(t + k) % ring_size];
		}
		memset(dst + n, 0, bytes - n);
		if (n < bytes && !atomic_load(&src_eof)) {
			atomic_fetch_add(&starved, 1);
		}
		atomic_store_explicit(&ring_tail, t + n, memory_order_release);
		break;
	}
	}
}

/* ---- sending ---- */

static uint8_t pkts[MAX_BATCH][PKT_MAX];

static void send_batch(uint64_t first, uint32_t n, uint64_t base)
{
	const uint32_t len = txc_packet_bytes(&core);
	struct iovec iov[MAX_BATCH];
	struct mmsghdr mm[MAX_BATCH];

	for (uint32_t k = 0; k < n; k++) {
		uint64_t idx = first + (uint64_t)k * pkt_words;

		txc_header(&core, pkts[k], idx);
		source_fill(idx, base, pkt_words, pkts[k] + TXC_HDR_BYTES);
	}
	if (use_gso) {
		/* one call: the segments are cut at `len` bytes, each keeps its own VITA header */
		static uint8_t big[MAX_BATCH * PKT_MAX];
		char ctl[CMSG_SPACE(sizeof(uint16_t))] = {0};
		struct msghdr mh = {0};
		struct cmsghdr *cm;
		uint16_t seg = (uint16_t)len;

		for (uint32_t k = 0; k < n; k++) {
			memcpy(big + (size_t)k * len, pkts[k], len);
		}
		iov[0] = (struct iovec){big, (size_t)n * len};
		mh.msg_name = &data_addr;
		mh.msg_namelen = addr_len;
		mh.msg_iov = iov;
		mh.msg_iovlen = 1;
		mh.msg_control = ctl;
		mh.msg_controllen = sizeof(ctl);
		cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_UDP;
		cm->cmsg_type = UDP_SEGMENT;
		cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
		memcpy(CMSG_DATA(cm), &seg, sizeof(seg));
		if (sendmsg(sock, &mh, 0) < 0) {
			atomic_fetch_add(&send_err, 1);
		}
	} else {
		uint32_t done = 0;

		for (uint32_t k = 0; k < n; k++) {
			iov[k] = (struct iovec){pkts[k], len};
			mm[k] = (struct mmsghdr){.msg_hdr = {.msg_name = &data_addr, .msg_namelen = addr_len,
							     .msg_iov = &iov[k], .msg_iovlen = 1}};
		}
		while (done < n) {
			int r = sendmmsg(sock, mm + done, n - done, 0);

			if (r < 0) {
				if (errno == EINTR) {
					continue;
				}
				if (errno == EAGAIN || errno == ENOBUFS) {
					nanosleep(&(struct timespec){0, 20000}, NULL);
					continue;
				}
				atomic_fetch_add(&send_err, 1);
				break;
			}
			done += (uint32_t)r;
		}
	}
	atomic_fetch_add(&sent_pkts, n);
	atomic_fetch_add(&sent_words, (uint64_t)n * pkt_words);
}

static uint64_t run_words(void)
{
	if (seconds > 0) {
		return (uint64_t)(seconds * (double)rate_msps * 1e6);
	}
	if (src_kind == SRC_FILE && !loop_file) {
		return file_words;
	}
	return UINT64_MAX;
}

static void *sender(void *arg)
{
	uint64_t base = 0, total = run_words();
	bool started = false;

	(void)arg;
	if (use_rt) {
		struct sched_param sp = {.sched_priority = 20};
		int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

		if (r != 0) {
			eprint("vrt_tx: SCHED_FIFO refused (%s), plain scheduling\n", strerror(r));
		}
	}
	while (!atomic_load(&stop_req)) {
		struct txp_plan pl;
		int64_t now, w;
		uint32_t maxpk = MAX_BATCH, n;
		bool eof = false;

		if (!atomic_load(&run_ok)) {
			nanosleep(&(struct timespec){0, 300000}, NULL);
			continue;
		}
		now = now_ns();
		pthread_mutex_lock(&mu);
		if (core.state != TXC_RUN) {
			pthread_mutex_unlock(&mu);
			break;
		}
		if (started && total != UINT64_MAX) {
			uint64_t left = base + total > core.pace.sent ? base + total - core.pace.sent : 0;

			if (left / pkt_words < maxpk) {
				maxpk = (uint32_t)(left / pkt_words);
			}
			eof = maxpk == 0;
		}
		if (src_kind == SRC_STDIN && atomic_load(&src_eof) && seconds <= 0 &&
		    atomic_load_explicit(&ring_head, memory_order_acquire) ==
			    atomic_load_explicit(&ring_tail, memory_order_acquire)) {
			eof = true;
		}
		if (eof) {
			pthread_mutex_unlock(&mu);
			break;
		}
		n = txc_plan(&core, now, maxpk, &pl);
		if (n == 0 && pl.credit_bound) {
			/* nothing to send until a status raises the credit: sleep on it (at most 1 ms) */
			struct timespec ts = {.tv_sec = (now + 1000000) / 1000000000LL,
					      .tv_nsec = (now + 1000000) % 1000000000LL};

			pthread_cond_timedwait(&cv, &mu, &ts);
			pthread_mutex_unlock(&mu);
			continue;
		}
		pthread_mutex_unlock(&mu);
		if (n != 0) {
			if (!started) {
				started = true;
				base = pl.first;
			}
			send_batch(pl.first, n, base);
		}
		w = pl.wake_ns;
		if (w - spin_ns > now_ns() + 5000) {
			struct timespec ts = {.tv_sec = (w - spin_ns) / 1000000000LL,
					      .tv_nsec = (w - spin_ns) % 1000000000LL};

			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		}
		while (now_ns() < w && !atomic_load(&stop_req)) {
			/* spin the last stretch */
		}
	}
	atomic_store(&sender_done, true);
	return NULL;
}

/* ---- control ---- */

static void send_ctl(const void *m, size_t len)
{
	if (sendto(sock, m, len, 0, (struct sockaddr *)&ctl_addr, addr_len) < 0 && errno != EAGAIN &&
	    errno != ECONNREFUSED) {
		eprint("vrt_tx: send: %s\n", strerror(errno));
	}
}

/* The address this host sends from towards the board (the one the board sees as the subscriber). */
static void local_addr_str(char *s, size_t n)
{
	struct sockaddr_storage la;
	socklen_t ll = sizeof(la);
	int fd = socket(ctl_addr.ss_family, SOCK_DGRAM, 0);

	snprintf(s, n, "?");
	if (fd < 0) {
		return;
	}
	if (connect(fd, (struct sockaddr *)&ctl_addr, addr_len) == 0 &&
	    getsockname(fd, (struct sockaddr *)&la, &ll) == 0) {
		void *a = la.ss_family == AF_INET6 ? (void *)&((struct sockaddr_in6 *)&la)->sin6_addr
						   : (void *)&((struct sockaddr_in *)&la)->sin_addr;

		inet_ntop(la.ss_family, a, s, (socklen_t)n);
	}
	close(fd);
}

static int connect_first(const char *host, const char *port, const char *iface)
{
	struct addrinfo hints = {.ai_socktype = SOCK_DGRAM}, *ai, *a;
	int ret = getaddrinfo(host, port, &hints, &ai), fd = -1;

	if (ret != 0) {
		eprint("vrt_tx: %s: %s\n", host, gai_strerror(ret));
		return -1;
	}
	for (int pass = 0; pass < 3 && fd < 0; pass++) {
		for (a = ai; a != NULL && fd < 0; a = a->ai_next) {
			bool v6 = a->ai_family == AF_INET6;
			bool ll = v6 && ((struct sockaddr_in6 *)a->ai_addr)->sin6_addr.s6_addr[0] == 0xfe &&
				  (((struct sockaddr_in6 *)a->ai_addr)->sin6_addr.s6_addr[1] & 0xc0) == 0x80;

			if ((pass == 0 && !ll) || (pass == 1 && (!v6 || ll)) || (pass == 2 && v6)) {
				continue;
			}
			fd = socket(a->ai_family, SOCK_DGRAM, 0);
			if (fd < 0) {
				continue;
			}
			memset(&ctl_addr, 0, sizeof(ctl_addr));
			memcpy(&ctl_addr, a->ai_addr, a->ai_addrlen);
			addr_len = a->ai_addrlen;
			if (ll && iface != NULL && ((struct sockaddr_in6 *)&ctl_addr)->sin6_scope_id == 0) {
				((struct sockaddr_in6 *)&ctl_addr)->sin6_scope_id = if_nametoindex(iface);
			}
			{
				char s[INET6_ADDRSTRLEN];
				void *addr = v6 ? (void *)&((struct sockaddr_in6 *)&ctl_addr)->sin6_addr
						: (void *)&((struct sockaddr_in *)&ctl_addr)->sin_addr;

				inet_ntop(a->ai_family, addr, s, sizeof(s));
				eprint("vrt_tx: using IPv%d, %s\n", v6 ? 6 : 4, s);
			}
		}
	}
	freeaddrinfo(ai);
	if (fd < 0) {
		eprint("vrt_tx: %s: no address to send to\n", host);
	}
	return fd;
}

static bool same_peer(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family) {
		return false;
	}
	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;

		return x->sin_addr.s_addr == y->sin_addr.s_addr && x->sin_port == y->sin_port;
	}
	{
		const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;

		return memcmp(&x->sin6_addr, &y->sin6_addr, 16) == 0 && x->sin6_port == y->sin6_port;
	}
}

static void set_port(struct sockaddr_storage *a, uint16_t port)
{
	if (a->ss_family == AF_INET) {
		((struct sockaddr_in *)a)->sin_port = htons(port);
	} else {
		((struct sockaddr_in6 *)a)->sin6_port = htons(port);
	}
}

static int exit_code;
static bool warned_depth, warned_lead;

static void report(const char *tag)
{
	struct txc_counts k;
	struct txp_st s;
	int64_t now = now_ns(), rtt = 0;
	uint64_t play, sent, credit, depth, lead_low;
	int64_t budget, min_lead;
	double dmax, dmaxs;
	bool short_;

	pthread_mutex_lock(&mu);
	s = core.last_st;
	k = txc_counts(&core);
	play = txp_pace_play(&core.pace, now);
	sent = core.pace.sent;
	credit = core.pace.credit;
	budget = txp_delay_budget_ns(&core.pace);
	lead_low = core.pace.lead_low;
	min_lead = core.pace.min_lead;
	depth = core.pace.depth;
	dmax = (double)txp_owd_max_ns(&core.pace) / 1e3;
	dmaxs = (double)txp_owd_stat_max_ns(&core.pace) / 1e3;
	short_ = txc_depth_short(&core);
	(void)txp_minq_get(&core.pace.rtt, &rtt);
	pthread_mutex_unlock(&mu);
	if (quiet) {
		return;
	}
	if (short_ && !warned_depth) {
		eprint("vrt_tx: WARNING: the delays measured (data up to %.0f us, status up to %.0f us, one way) are "
		       "over what depth %llu and the line carry (%.0f us together); expect underruns\n", dmax, dmaxs,
		       (unsigned long long)depth, (double)budget / 1e3);
		warned_depth = true;
	}
	if (lead_low != 0 && !warned_lead) {
		eprint("vrt_tx: WARNING: the lead fell under one window + 1000 words (%llu times, least %lld)\n",
		       (unsigned long long)lead_low, (long long)min_lead - 32768);
		warned_lead = true;
	}
	eprint("vrt_tx: %s sent %llu words, lead %lld, credit room %lld, rtt %.0f us, underruns %u, lost %llu, "
	       "late %llu, stale %llu, resyncs %llu, starved %llu, send errors %llu, status %s\n",
	       tag, (unsigned long long)atomic_load(&sent_words), (long long)sent - (long long)play,
	       (long long)credit - (long long)sent, (double)rtt / 1e3, s.status.underruns,
	       (unsigned long long)s.status.lost, (unsigned long long)s.status.late,
	       (unsigned long long)s.status.stale, (unsigned long long)k.resyncs,
	       (unsigned long long)atomic_load(&starved), (unsigned long long)atomic_load(&send_err),
	       (s.flags & TXP_ST_RUNNING) ? "running" : "idle");
}

static int parse_source(const char *s, int64_t *amp_out, int *amp_given)
{
	*amp_given = 0;
	if (strncmp(s, "tone:", 5) == 0) {
		char *e;
		double khz = strtod(s + 5, &e);

		src_kind = SRC_TONE;
		*amp_out = 1;
		if (e == s + 5) {
			return -1;
		}
		if (*e == ':') {
			*amp_out = strtol(e + 1, &e, 10);
		}
		if (*e != '\0' || *amp_out < 1 || *amp_out > 127) {
			return -1;
		}
		tone.freq_hz = (int64_t)(khz * 1000.0);
		return 0;
	}
	if (strcmp(s, "board") == 0) {
		src_kind = SRC_BOARD;
		return 0;
	}
	if (strncmp(s, "file:", 5) == 0) {
		src_kind = SRC_FILE;
		return 0;
	}
	if (strcmp(s, "stdin") == 0) {
		src_kind = SRC_STDIN;
		return 0;
	}
	return -1;
}

static int load_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	long sz;

	if (f == NULL) {
		eprint("vrt_tx: %s: %s\n", path, strerror(errno));
		return -1;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || (size_t)sz % sample_bytes != 0 || (size_t)sz > (1UL << 30)) {
		eprint("vrt_tx: %s: size %ld is not a whole number (1..2^30 bytes) of samples\n", path, sz);
		fclose(f);
		return -1;
	}
	file_buf = malloc((size_t)sz);
	if (file_buf == NULL || fread(file_buf, 1, (size_t)sz, f) != (size_t)sz) {
		eprint("vrt_tx: %s: read failed\n", path);
		fclose(f);
		return -1;
	}
	fclose(f);
	file_words = (size_t)sz / sample_bytes;
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option lo[] = {
		{"to", 1, 0, 't'}, {"iface", 1, 0, 'i'}, {"rate", 1, 0, 'r'}, {"fmt", 1, 0, 'f'},
		{"source", 1, 0, 's'}, {"seconds", 1, 0, 'S'}, {"rf", 0, 0, 'R'}, {"verify", 0, 0, 'V'},
		{"loop", 0, 0, 'l'}, {"port", 1, 0, 'p'}, {"dport", 1, 0, 'd'}, {"max-amp", 1, 0, 'm'},
		{"allow-amp", 0, 0, 'A'}, {"gso", 0, 0, 'g'}, {"rt", 0, 0, 'T'}, {"spin-us", 1, 0, 'n'},
		{"arm-wait", 1, 0, 'a'}, {"pkt-words", 1, 0, 'w'}, {"max-mbps", 1, 0, 'o'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
	const char *to = NULL, *iface = NULL, *srcs = NULL, *port = "4992";
	int dport = 4994, max_amp = 4, opt, amp_given, arm_wait_s = 60;
	int64_t amp = 1, t_next_rep;
	bool allow_amp = false, had_run = false;
	pthread_t th, rd;
	uint32_t nonce;
	struct sigaction sa = {.sa_handler = on_sig};

	while ((opt = getopt_long(argc, argv, "q", lo, NULL)) != -1) {
		switch (opt) {
		case 't': to = optarg; break;
		case 'i': iface = optarg; break;
		case 'r': rate_msps = (uint32_t)atoi(optarg); break;
		case 'f':
			if (strcmp(optarg, "iq8") == 0) {
				fmt = TXP_FMT_IQ8;
			} else if (strcmp(optarg, "w32") == 0) {
				fmt = TXP_FMT_WORD32;
			} else {
				usage();
				return 2;
			}
			break;
		case 's': srcs = optarg; break;
		case 'S': seconds = atof(optarg); break;
		case 'R': go_flags |= TXP_GO_RF; break;
		case 'V': go_flags |= TXP_GO_VERIFY; break;
		case 'l': loop_file = true; break;
		case 'p': port = optarg; break;
		case 'd': dport = atoi(optarg); break;
		case 'm': max_amp = atoi(optarg); break;
		case 'A': allow_amp = true; break;
		case 'g': use_gso = true; break;
		case 'T': use_rt = true; break;
		case 'n': spin_ns = (int64_t)atoi(optarg) * 1000; break;
		case 'w': pkt_words = (uint32_t)atoi(optarg); break;
		case 'o': max_mbps = (uint32_t)atoi(optarg); break;
		case 'a': arm_wait_s = atoi(optarg); break;
		case 'q': quiet = true; break;
		default: usage(); return 2;
		}
	}
	if (to == NULL || srcs == NULL || (rate_msps != 20 && rate_msps != 40)) {
		usage();
		return 2;
	}
	if (parse_source(srcs, &amp, &amp_given) != 0) {
		usage();
		return 2;
	}
	if ((go_flags & TXP_GO_VERIFY) != 0 && src_kind != SRC_BOARD) {
		eprint("vrt_tx: warning: --verify reads the board's bank back against the board's own tone; "
		       "with this source it will count wrong words (use --source board)\n");
	}
	sample_bytes = fmt == TXP_FMT_IQ8 ? 2U : 4U;
	if (pkt_words == 0) {
		pkt_words = 1404U / sample_bytes;
	}
	if (pkt_words * sample_bytes + HDR_BYTES > PKT_MAX || pkt_words < 2 || (fmt == TXP_FMT_IQ8 && (pkt_words & 1U) != 0)) {
		eprint("vrt_tx: --pkt-words must fit a 1500 byte frame (even for iq8)\n");
		return 2;
	}
	{
		/* the stream must fit the line cap: packets a second x wire bits */
		double pps = (double)rate_msps * 1e6 / (double)pkt_words;
		double mbps = pps * (double)(HDR_BYTES + pkt_words * sample_bytes + TXP_WIRE_OVERHEAD) * 8.0 / 1e6;

		if (mbps > (double)max_mbps) {
			eprint("vrt_tx: %u MS/s %s needs %.0f Mb/s on the wire, over --max-mbps %u: it cannot be sent\n",
			       rate_msps, fmt == TXP_FMT_IQ8 ? "iq8" : "w32", mbps, max_mbps);
			return 2;
		}
	}
	if (max_amp < 1 || max_amp > 127) {
		eprint("vrt_tx: --max-amp is 1..127\n");
		return 2;
	}
	if (src_kind == SRC_TONE) {
		if (amp > max_amp && !allow_amp) {
			eprint("vrt_tx: amplitude %lld is above --max-amp %d: pass --allow-amp if you mean it\n",
			       (long long)amp, max_amp);
			return 2;
		}
		txsrc_tone_init(&tone, rate_msps * 1000000U, tone.freq_hz, (int)amp);
	} else if (src_kind == SRC_FILE) {
		int peak;

		if (strncmp(srcs, "file:", 5) != 0 || load_file(srcs + 5) != 0) {
			return 2;
		}
		peak = txsrc_peak(file_buf, file_words, fmt);
		if (peak > max_amp && !allow_amp) {
			eprint("vrt_tx: the file peaks at %d, above --max-amp %d: pass --allow-amp if you mean it\n",
			       peak, max_amp);
			return 2;
		}
	} else if (src_kind == SRC_BOARD) {
		/* the board's tone: 4 DAC counts (iq8 amplitude 1), below any --max-amp */
	} else {
		/* stdin cannot be scanned in advance */
		if (!allow_amp) {
			eprint("vrt_tx: stdin is not scanned for its amplitude: pass --allow-amp (the board does not clip)\n");
			return 2;
		}
		ring = malloc(ring_size);
		if (ring == NULL) {
			return 2;
		}
	}
	if (seconds <= 0 && (src_kind == SRC_TONE || src_kind == SRC_BOARD)) {
		eprint("vrt_tx: a tone needs --seconds\n");
		return 2;
	}

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sock = connect_first(to, port, iface);
	if (sock < 0) {
		return 1;
	}
	data_addr = ctl_addr;
	set_port(&data_addr, (uint16_t)dport);
	{
		int sb = 1 << 20;

		(void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));
	}
	{
		pthread_condattr_t ca;

		pthread_condattr_init(&ca);
		pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
		pthread_cond_init(&cv, &ca);
	}
	srand((unsigned)now_ns());
	arm_wait_ns = (int64_t)arm_wait_s * 1000000000LL;
	nonce = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
	txc_init(&core, rate_msps, fmt, go_flags, seconds > 0 ? (uint32_t)(seconds + 2.0) : 0U, nonce, pkt_words,
		 max_mbps);
	core.arm_wait_ns = arm_wait_ns;
	if (src_kind == SRC_STDIN) {
		pthread_create(&rd, NULL, stdin_reader, NULL);
	}
	t_next_rep = now_ns() + 1000000000LL;
	pthread_create(&th, NULL, sender, NULL);

	for (;;) {
		struct pollfd pf = {.fd = sock, .events = POLLIN};
		int64_t now = now_ns(), wake;
		uint8_t d[512];
		int n;
		enum txc_state st;

		pthread_mutex_lock(&mu);
		if ((atomic_load(&stop_req) || atomic_load(&sender_done)) && core.state == TXC_RUN) {
			txc_stop(&core);
		}
		while ((n = txc_poll(&core, now, d, sizeof(d))) > 0) {
			pthread_mutex_unlock(&mu);
			send_ctl(d, (size_t)n);
			pthread_mutex_lock(&mu);
		}
		st = core.state;
		wake = txc_wake(&core);
		pthread_mutex_unlock(&mu);
		if (st == TXC_RUN && !atomic_load(&run_ok)) {
			had_run = true;
			pthread_mutex_lock(&mu);
			eprint("vrt_tx: run accepted: stream id %08x, max %u words, depth %u, group %u%s\n",
			       core.ak.stream_id, core.ak.max_words, core.ak.depth, core.ak.group,
			       (core.ak.flags & TXP_AK_RF) ? ", RF on" : "");
			pkt_words = core.pkt_words;
			pthread_mutex_unlock(&mu);
			atomic_store(&run_ok, true);
		}
		if (st == TXC_DONE || st == TXC_FAILED) {
			break;
		}
		if (st < TXC_RUN) {
			/* the line a second counts from the run's start: after a wait for the arming it would print the
			 * seconds of the wait at once (three lines of "sent 0 words"), and the stale time would wake
			 * the loop at once, every pass, for the whole wait */
			t_next_rep = now + 1000000000LL;
		}
		if (now >= t_next_rep && st >= TXC_RUN) {
			report("run:");
			t_next_rep += 1000000000LL;
		}
		if (t_next_rep < wake) {
			wake = t_next_rep;
		}
		{
			int ms = wake > now ? (int)((wake - now) / 1000000) : 0;

			if (poll(&pf, 1, ms > 5 ? 5 : ms) > 0) {
				for (;;) {
					struct sockaddr_storage from;
					socklen_t fl = sizeof(from);
					ssize_t r = recvfrom(sock, d, sizeof(d), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
					int64_t tn;

					if (r < 0) {
						break;
					}
					if (!same_peer(&from, &ctl_addr)) {
						continue;
					}
					tn = now_ns();
					pthread_mutex_lock(&mu);
					unsigned ev = txc_rx(&core, tn, d, (size_t)r);

					if (ev & TXC_EV_STATUS) {
						pthread_cond_signal(&cv);
					}
					if (ev & TXC_EV_ARMWAIT) {
						char me[INET6_ADDRSTRLEN];

						local_addr_str(me, sizeof(me));
						eprint("vrt_tx: RF refused: waiting for `tx arm <minutes>` on the board's UART "
						       "(subscriber %s), asking again every second for up to %d s\n", me, arm_wait_s);
					}
					pthread_mutex_unlock(&mu);
				}
			}
		}
	}

	atomic_store(&stop_req, true);
	pthread_join(th, NULL);
	if (src_kind == SRC_STDIN) {
		pthread_cancel(rd);
	}
	if (core.have_cookie) {
		uint8_t m[12];
		int len;

		txc_vrts(&core, m, &len);
		memcpy(m, "VRTX", 4);
		send_ctl(m, 12);
	}
	if (core.fail == TXC_FAIL_BUSY) {
		eprint("vrt_tx: another host has the stream or the board is busy\n");
		exit_code = 3;
	} else if (core.fail == TXC_FAIL_REFUSED) {
		eprint("vrt_tx: the board refused TXGO (code %u)\n", core.ak.code);
		exit_code = 4;
	} else if (core.fail == TXC_FAIL_NOCONT) {
		eprint("vrt_tx: no answer from the board\n");
		exit_code = 1;
	} else if (core.fail == TXC_FAIL_ENDED) {
		eprint("vrt_tx: the board ended the run (status ended or not running for 200 ms, or silent for 2 s)\n");
		exit_code = 1;
	}
	if (had_run) {
		report("end:");
	}
	if (exit_code == 0 && core.have_first) {
		struct txc_counts k = txc_counts(&core);

		if (k.underruns != 0 || k.lost != 0 || k.late != 0 || k.resyncs != 0 ||
		    atomic_load(&send_err) != 0 || atomic_load(&starved) != 0) {
			eprint("vrt_tx: NOT CLEAN: underruns +%u, lost +%llu, late +%llu, resyncs %llu\n", k.underruns,
			       (unsigned long long)k.lost, (unsigned long long)k.late, (unsigned long long)k.resyncs);
			exit_code = 1;
		}
		if (core.state != TXC_DONE) {
			eprint("vrt_tx: the board did not report the end within 3 s\n");
		}
	} else if (exit_code == 0) {
		eprint("vrt_tx: the engine never ran\n");
		exit_code = 1;
	}
	return exit_code;
}
