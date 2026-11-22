// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vrt_udp: receive the ESP32-S31's VITA 49.2 stream over UDP and write it as raw cs8.
 *
 *   vrt_udp <board address or name> [-p port] [-r samples/s] [-f Hz] [-g gain|auto] [-t seconds]
 *                                   [-o file|-] [-b MiB] [-T] [-z] [-q]
 *
 * -z asks for IPv6 packets with a zero UDP checksum (UDP_NO_CHECK6_RX), if the board is built for it.
 *
 * IPv6 first: names and literals are tried AAAA (link-local first) before A, and the family used
 * is printed. A link-local address needs its scope: fe80::1234%eth0.
 *
 * The subscription (the board streams only to a host that has shown it can receive): "VRTS", the
 * board's "VRTC" + cookie, "VRTS" + cookie every 0.25 s, "VRTK" when it subscribes, "VRTB" when
 * another host has the stream (exit 3; -T takes it over with "VRTT"), "VRTX" + cookie at the end.
 * Settings (-r -f -g) go as a VITA 49.2 command packet once subscribed, repeated until the board
 * acknowledges (five tries, then exit 4). Counting starts when they are in force.
 *
 * Every packet is checked (counter, stream ID, time against the sample index, trailer). Interleaved
 * int8 I,Q go to the file, or to stdout with "-o -". A gap is filled with zeros so that time stays
 * continuous. Status goes to stderr. Exit status 0 only when nothing was lost: nothing missing on
 * the network (counter), nothing the board dropped (sample loss flag) or restarted, no kernel
 * drop (SO_RXQ_OVFL), no write error, no overflow of the writer's ring, the capture as long as -t
 * asked for, the board never silent for 5 s.
 *
 * Build, from the workspace root (gcc and libvrt's sources, no root, no other library):
 *   gcc -O2 -Wall -pthread -o vrt_udp esp-sdr-zephyr/apps/stream_eth/tools/vrt_udp.c \
 *       -Imodules/lib/libvrt/include -Imodules/lib/libvrt/src modules/lib/libvrt/src/vrt_*.c \
 *       modules/lib/libvrt/src/timestamp_to_calendar.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <getopt.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
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
#include <fcntl.h>
#include <poll.h>

#include <vrt/vrt_init.h>
#include <vrt/vrt_read.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>

#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

#define STREAM_ID 0x5d5d0001U
#define BATCH     64
#define PKT_MAX   2048
#define SLOT_DATA 1536
#define MAX_FILL_S 5

struct slot {
	uint32_t len;   /* sample bytes, 0 for a gap */
	uint64_t zeros; /* samples (pairs) of zeros, for a gap */
	uint8_t data[SLOT_DATA];
};

static struct slot *ring;
static size_t ring_n;
static _Atomic size_t head, tail;

static struct {
	_Atomic uint64_t packets, bytes, context, other, bad, pkts_lost, board_loss_pkts;
	_Atomic uint64_t board_loss_samples, net_loss_samples, overrange, kernel_drops;
	_Atomic uint64_t late, writer_drops, filled, samples, wr_bytes, resyncs, write_err;
} st;

static _Atomic bool stop, counting, subscribed, busy, acked, silent;
static _Atomic uint64_t cookie_a;
static _Atomic bool have_cookie;
static _Atomic int64_t last_pkt_us;
static double want_rate;
static int sock = -1;
static _Atomic double rate_a;
static bool quiet;
static int out_fd = -1;
static bool out_stdout;

static void on_sig(int s)
{
	(void)s;
	atomic_store(&stop, true);
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void eprint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void eprint(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ---- writer thread ---- */

static int write_all(const uint8_t *p, size_t n)
{
	int64_t give_up = 0;

	while (n > 0) {
		ssize_t w = write(out_fd, p, n);

		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				struct pollfd pf = {.fd = out_fd, .events = POLLOUT};

				/* a reader that does not read: wait, but not forever once stopped */
				if (atomic_load(&stop)) {
					if (give_up == 0) {
						give_up = (int64_t)(now_s() * 1000) + 2000;
					} else if ((int64_t)(now_s() * 1000) > give_up) {
						atomic_fetch_add(&st.write_err, 1);
						return -1;
					}
				}
				poll(&pf, 1, 100);
				continue;
			}
			atomic_fetch_add(&st.write_err, 1);
			return -1;
		}
		p += w;
		n -= (size_t)w;
		atomic_fetch_add(&st.wr_bytes, (uint64_t)w);
	}
	return 0;
}

static void *writer(void *arg)
{
	static uint8_t zeros[65536];
	static uint8_t acc[262144];
	size_t fill = 0;

	(void)arg;
	for (;;) {
		size_t t = atomic_load_explicit(&tail, memory_order_relaxed);
		size_t h = atomic_load_explicit(&head, memory_order_acquire);

		if (t == h) {
			if (fill != 0 && out_fd >= 0) {
				if (write_all(acc, fill) != 0) {
					atomic_store(&stop, true);
				}
				fill = 0;
			}
			if (atomic_load(&stop) && atomic_load_explicit(&head, memory_order_acquire) == t) {
				break;
			}
			nanosleep(&(struct timespec){0, 200000}, NULL);
			continue;
		}
		struct slot *s = &ring[t % ring_n];

		if (out_fd >= 0) {
			if (s->len != 0) {
				if (fill + s->len > sizeof(acc)) {
					if (write_all(acc, fill) != 0) {
						atomic_store(&stop, true);
					}
					fill = 0;
				}
				memcpy(acc + fill, s->data, s->len);
				fill += s->len;
			} else {
				uint64_t bytes = s->zeros * 2U;

				if (fill != 0) {
					if (write_all(acc, fill) != 0) {
						atomic_store(&stop, true);
					}
					fill = 0;
				}
				while (bytes > 0) {
					size_t n = bytes < sizeof(zeros) ? (size_t)bytes : sizeof(zeros);

					if (write_all(zeros, n) != 0) {
						atomic_store(&stop, true);
						break;
					}
					bytes -= n;
				}
			}
		}
		atomic_store_explicit(&tail, t + 1, memory_order_release);
	}
	return NULL;
}

/* A slot for the receiver to fill, or NULL when the writer is behind. */
static struct slot *slot_get(void)
{
	size_t h = atomic_load_explicit(&head, memory_order_relaxed);

	if (h - atomic_load_explicit(&tail, memory_order_acquire) >= ring_n) {
		return NULL;
	}
	return &ring[h % ring_n];
}

static void slot_put(void)
{
	atomic_store_explicit(&head, atomic_load_explicit(&head, memory_order_relaxed) + 1,
			      memory_order_release);
}

static void put_gap(uint64_t samples)
{
	/* a gap is however many slots of one: the count is a 64 bit number */
	struct slot *s = slot_get();

	if (s == NULL) {
		atomic_fetch_add(&st.writer_drops, 1);
		return;
	}
	s->len = 0;
	s->zeros = samples;
	slot_put();
}

/* ---- receive ---- */

static inline uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

struct rxstate {
	bool have_count, have_index;
	uint8_t last_count;
	uint64_t next_index;
	uint64_t last_kernel;
	bool kernel_seen;
	double rate;
};

#define RENEW_S 0.25

static bool settings_wanted, want_freq_set, want_gain_set;
static double want_freq, want_gain;
static _Atomic bool ctx_ok, have_ack_epoch, ctx_has_epoch;
static _Atomic int ack_epoch;
static _Atomic int64_t counting_ms;

static void update_counting(void)
{
	double r = atomic_load(&rate_a);

	if (atomic_load(&counting) || r <= 0) {
		return;
	}
	if (!settings_wanted || atomic_load(&ctx_ok)) {
		atomic_store(&counting_ms, (int64_t)(now_s() * 1000));
		atomic_store(&last_pkt_us, (int64_t)(now_s() * 1000));
		atomic_store(&counting, true);
	}
}

static void handle_context(const uint8_t *buf, size_t len)
{
	uint32_t words[128];
	struct vrt_packet p;

	if (len % 4 != 0 || len / 4 > 128) {
		atomic_fetch_add(&st.bad, 1);
		return;
	}
	memcpy(words, buf, len);
	vrt_from_big_endian(words, (int32_t)(len / 4));
	vrt_init_packet(&p);
	if (vrt_read_packet(words, (int32_t)(len / 4), &p, true) < 0) {
		atomic_fetch_add(&st.bad, 1);
		return;
	}
	atomic_store(&subscribed, true);
	atomic_fetch_add(&st.context, 1);
	if (p.if_context.has.state_and_event_indicators) {
		atomic_store(&ctx_has_epoch, true);
	}
	if (p.if_context.has.sample_rate && p.if_context.sample_rate > 0) {
		double r = p.if_context.sample_rate;

		if (r != atomic_load(&rate_a)) {
			eprint("vrt_udp: context: rf %.0f Hz, rate %.0f S/s, bandwidth %.0f Hz\n",
			       p.if_context.rf_reference_frequency, r, p.if_context.bandwidth);
		}
		atomic_store(&rate_a, r);
		if (atomic_load(&acked)) {
			/* a context after the acknowledge: the new run's (or the same one, if nothing changed),
			 * and it must show all of what was asked for
			 */
			bool has_ep = p.if_context.has.state_and_event_indicators;
			bool ok = (want_rate == 0 || r == want_rate) &&
				  (!want_freq_set || (p.if_context.has.rf_reference_frequency &&
						      fabs(p.if_context.rf_reference_frequency - want_freq) < 100000.0)) &&
				  (!want_gain_set || (p.if_context.has.gain && p.if_context.gain.stage1 == (float)want_gain));

			if (has_ep) {
				/* the board numbers its settings: this run is the one that was asked for when its
				 * epoch is the acknowledged one (the tolerance above is for a board without)
				 */
				ok = atomic_load(&have_ack_epoch) &&
				     p.if_context.state_and_event_indicators.user_defined == atomic_load(&ack_epoch);
			}
			atomic_store(&ctx_ok, ok);
		}
		update_counting();
	}
}

static _Atomic bool refused;

static void handle_ack(const uint8_t *buf, size_t len)
{
	uint32_t words[128];
	struct vrt_packet p;
	struct vrt_command c;

	if (len % 4 != 0 || len / 4 > 128) {
		return;
	}
	memcpy(words, buf, len);
	vrt_from_big_endian(words, (int32_t)(len / 4));
	vrt_init_packet(&p);
	vrt_init_command(&c);
	p.command = &c;
	if (vrt_read_packet(words, (int32_t)(len / 4), &p, true) < 0) {
		return;
	}
	if (c.cam.query_state) {
		/* the state after the command, with the epoch of the settings */
		if (p.if_context.has.state_and_event_indicators) {
			atomic_store(&ack_epoch, p.if_context.state_and_event_indicators.user_defined);
			atomic_store(&have_ack_epoch, true);
			atomic_store(&ctx_ok, false);
		}
		return;
	}
	if (!c.cam.execution) {
		return;
	}
	if (c.cam.errors || !c.cam.scheduled_or_executed) {
		eprint("vrt_udp: the board did not execute the command (executed %d, errors %d: frequency %d, gain %d, "
		       "rate %d, format %d)\n",
		       c.cam.scheduled_or_executed, c.cam.errors, c.errors.has.rf_reference_frequency,
		       c.errors.has.gain, c.errors.has.sample_rate, c.errors.has.data_packet_payload_format);
		atomic_store(&refused, true);
	} else {
		eprint("vrt_udp: the board executed the command\n");
		atomic_store(&acked, true);
		atomic_store(&ctx_ok, false);
	}
}

static void handle_data(struct rxstate *rx, const uint8_t *b, size_t len)
{
	uint32_t w0 = be32(b);
	size_t i = 4;
	bool trailer = (w0 >> 26) & 1U;
	unsigned int tsi = (w0 >> 22) & 3U, tsf = (w0 >> 20) & 3U, count = (w0 >> 16) & 15U;
	uint32_t sid, sec = 0;
	uint64_t ps = 0, idx;
	size_t body_len;
	const uint8_t *body;
	bool loss = false, over = false;
	double r = atomic_load(&rate_a);
	struct slot *s;

	if (!atomic_load(&counting)) {
		/* before the settings are in force: not the capture */
		rx->have_index = rx->have_count = false;
		return;
	}
	if ((w0 & 0xffffU) * 4U != len || len < 28) {
		atomic_fetch_add(&st.bad, 1);
		return;
	}
	sid = be32(b + i);
	i += 4;
	if ((w0 >> 27) & 1U) {
		i += 8;
	}
	if (tsi != 0) {
		sec = be32(b + i);
		i += 4;
	}
	if (tsf != 0) {
		ps = (uint64_t)be32(b + i) << 32 | be32(b + i + 4);
		i += 8;
	}
	if (sid != STREAM_ID || tsi == 0 || tsf != 2 || !trailer || i + 4 > len) {
		atomic_fetch_add(&st.bad, 1);
		return;
	}
	body = b + i;
	body_len = len - i - 4;
	{
		uint32_t t = be32(b + len - 4);

		loss = (t >> 12) & 1U && (t >> 24) & 1U;
		over = (t >> 13) & 1U && (t >> 25) & 1U;
	}
	if (r <= 0 || body_len == 0 || body_len % 4 != 0 || body_len > SLOT_DATA) {
		/* before the first context packet: not counted */
		if (r > 0) {
			atomic_fetch_add(&st.bad, 1);
		}
		return;
	}
	atomic_store(&last_pkt_us, (int64_t)(now_s() * 1000));
	if (rx->have_index && r != rx->rate) {
		/* a new rate: a new time axis */
		rx->have_index = rx->have_count = false;
	}
	rx->rate = r;
	atomic_fetch_add(&st.packets, 1);
	atomic_fetch_add(&st.bytes, len);
	if (over) {
		atomic_fetch_add(&st.overrange, 1);
	}
	idx = (uint64_t)sec * (uint64_t)r +
	      (uint64_t)(((unsigned __int128)ps * (uint64_t)r + 500000000000ULL) / 1000000000000ULL);
	if (rx->have_count && ((count - rx->last_count) & 15U) != 1U) {
		/* the counter is the network's: missing packets (up to 15 told apart) */
		atomic_fetch_add(&st.pkts_lost, ((count - rx->last_count - 1U) & 15U) ? (count - rx->last_count - 1U) & 15U : 16U);
	}
	rx->last_count = (uint8_t)count;
	rx->have_count = true;
	if (loss) {
		atomic_fetch_add(&st.board_loss_pkts, 1);
	}
	if (rx->have_index && idx != rx->next_index) {
		if (idx > rx->next_index) {
			uint64_t gap = idx - rx->next_index;

			if (loss) {
				atomic_fetch_add(&st.board_loss_samples, gap);
			} else {
				atomic_fetch_add(&st.net_loss_samples, gap);
			}
			if (gap <= (uint64_t)r * MAX_FILL_S) {
				put_gap(gap);
				atomic_fetch_add(&st.filled, gap);
			}
		} else if (rx->next_index - idx > body_len / 2U * 4U) {
			/* the time went back by more than a few packets: the board restarted or
			 * the stream was retuned; the time axis starts again, and that is a loss
			 */
			atomic_fetch_add(&st.resyncs, 1);
		} else {
			/* behind: a repeat or reordered packet, its samples are in already */
			atomic_fetch_add(&st.late, 1);
			return;
		}
	}
	rx->next_index = idx + body_len / 2U;
	rx->have_index = true;
	atomic_fetch_add(&st.samples, body_len / 2U);
	s = slot_get();
	if (s == NULL) {
		atomic_fetch_add(&st.writer_drops, 1);
		/* the writer is behind: these samples are not in the file; the time does not move on, so
		 * that the next packet that fits is filled up to with zeros
		 */
		rx->next_index = idx;
		return;
	}
	s->len = (uint32_t)body_len;
	memcpy(s->data, body, body_len);
	slot_put();
}

static void *receiver(void *arg)
{
	static uint8_t bufs[BATCH][PKT_MAX];
	static char ctrl[BATCH][64];
	struct mmsghdr m[BATCH];
	struct iovec iov[BATCH];
	struct rxstate rx = {0};

	(void)arg;
	/* SO_RXQ_OVFL counts from the socket's first datagram: its baseline is 0 */
	rx.kernel_seen = true;
	rx.last_kernel = 0;
	for (int k = 0; k < BATCH; k++) {
		iov[k].iov_base = bufs[k];
		iov[k].iov_len = PKT_MAX;
	}
	while (!atomic_load(&stop)) {
		int n;

		for (int k = 0; k < BATCH; k++) {
			memset(&m[k].msg_hdr, 0, sizeof(m[k].msg_hdr));
			m[k].msg_hdr.msg_iov = &iov[k];
			m[k].msg_hdr.msg_iovlen = 1;
			m[k].msg_hdr.msg_control = ctrl[k];
			m[k].msg_hdr.msg_controllen = sizeof(ctrl[k]);
		}
		n = recvmmsg(sock, m, BATCH, MSG_WAITFORONE, NULL);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == ECONNREFUSED) {
				continue;
			}
			eprint("vrt_udp: recvmmsg: %s\n", strerror(errno));
			atomic_store(&stop, true);
			break;
		}
		for (int k = 0; k < n; k++) {
			size_t len = m[k].msg_len;
			const uint8_t *b = bufs[k];
			struct cmsghdr *c;

			for (c = CMSG_FIRSTHDR(&m[k].msg_hdr); c; c = CMSG_NXTHDR(&m[k].msg_hdr, c)) {
				if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
					uint32_t d;

					memcpy(&d, CMSG_DATA(c), 4);
					/* a count since the socket opened: the rise is what was lost */
					if (rx.kernel_seen) {
						atomic_fetch_add(&st.kernel_drops, (uint32_t)(d - rx.last_kernel));
					}
					rx.last_kernel = d;
					rx.kernel_seen = true;
				}
			}
			if (len >= 4 && memcmp(b, "VRT", 3) == 0) {
				/* the board's subscription messages (a data packet never starts 'V') */
				if (b[3] == 'C' && len == 12) {
					uint64_t ck;

					memcpy(&ck, b + 4, 8);
					atomic_store(&cookie_a, ck);
					atomic_store(&have_cookie, true);
				} else if (b[3] == 'K') {
					atomic_store(&subscribed, true);
				} else if (b[3] == 'B') {
					atomic_store(&busy, true);
				}
				continue;
			}
			if (len < 4 || (m[k].msg_hdr.msg_flags & MSG_TRUNC)) {
				atomic_fetch_add(&st.bad, 1);
				continue;
			}
			switch (b[0] >> 4) {
			case 1:
			case 0:
				handle_data(&rx, b, len);
				break;
			case 4:
			case 5:
				handle_context(b, len);
				break;
			case 6:
				handle_ack(b, len);
				break;
			default:
				atomic_fetch_add(&st.other, 1);
			}
		}
	}
	return NULL;
}

/* ---- control ---- */

static void send_cmd(double freq, bool has_freq, double gain, bool has_gain, double rate_req,
		     bool has_rate)
{
	static uint32_t msg_id = 1;
	uint32_t buf[64];
	struct vrt_packet p;
	struct vrt_command c;
	int32_t w;

	vrt_init_packet(&p);
	vrt_init_command(&c);
	p.command = &c;
	p.header.packet_type = VRT_PT_COMMAND;
	p.fields.stream_id = STREAM_ID;
	c.message_id = msg_id++;
	c.cam.action_mode = VRT_AM_EXECUTE;
	c.cam.execution = true;
	/* ask for the error words: a refusal says which setting, and cannot pass for an execution */
	c.cam.errors = true;
	/* and the state after it, in a second acknowledge: the epoch of the settings now */
	c.cam.query_state = true;
	if (has_freq) {
		p.if_context.has.rf_reference_frequency = true;
		p.if_context.rf_reference_frequency = freq;
	}
	if (has_gain) {
		p.if_context.has.gain = true;
		p.if_context.gain.stage1 = (float)gain;
	}
	if (has_rate) {
		p.if_context.has.sample_rate = true;
		p.if_context.sample_rate = rate_req;
	}
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	w = vrt_write_packet(&p, buf, 64, true);
	if (w < 0) {
		eprint("vrt_udp: cannot encode the command (%d)\n", w);
		return;
	}
	vrt_to_big_endian(buf, w);
	if (send(sock, buf, (size_t)w * 4U, 0) < 0 && errno != ECONNREFUSED) {
		eprint("vrt_udp: send: %s\n", strerror(errno));
	}
}

static bool zero_csum;

static void send_sub(bool takeover)
{
	uint8_t m[13];

	memcpy(m, takeover && atomic_load(&have_cookie) ? "VRTT" : "VRTS", 4);
	if (!atomic_load(&have_cookie)) {
		(void)send(sock, m, 4, 0);
		return;
	}
	{
		uint64_t ck = atomic_load(&cookie_a);

		memcpy(m + 4, &ck, 8);
	}
	m[12] = zero_csum ? 1 : 0;
	(void)send(sock, m, zero_csum ? 13 : 12, 0);
}

static void usage(void)
{
	eprint("usage: vrt_udp <board address> [-p port] [-r samples/s] [-f Hz] [-g gain|auto]\n"
	       "                [-t seconds] [-o file|-] [-b MiB] [-T] [-z] [-q]\n");
}

/* IPv6 first, link-local before the rest, then IPv4. */
static int connect_first(const char *host, const char *port)
{
	struct addrinfo hints = {.ai_socktype = SOCK_DGRAM}, *ai, *a;
	int ret = getaddrinfo(host, port, &hints, &ai), fd = -1;

	if (ret != 0) {
		eprint("vrt_udp: %s: %s\n", host, gai_strerror(ret));
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
			if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
				close(fd);
				fd = -1;
			}
			if (fd >= 0) {
				char s[INET6_ADDRSTRLEN];
				void *addr = v6 ? (void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr
						: (void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr;

				inet_ntop(a->ai_family, addr, s, sizeof(s));
				eprint("vrt_udp: using IPv%d, %s\n", v6 ? 6 : 4, s);
			}
		}
	}
	freeaddrinfo(ai);
	if (fd < 0) {
		eprint("vrt_udp: %s: no address to connect to\n", host);
	}
	return fd;
}

int main(int argc, char **argv)
{
	const char *host, *outname = NULL, *port = "4992";
	double freq = 0, gain = 0, rate_req = 0, secs = 0;
	bool has_freq = false, has_gain = false, has_rate = false, takeover = false;
	size_t ring_mib = 64;
	pthread_t rt, wt;
	int opt, one = 1, rcv = 0, rc = 0, tries = 0;
	socklen_t optlen = sizeof(rcv);
	double t0, tprev, tsub, tlast_cmd = 0;
	uint64_t prev_packets = 0, prev_samples = 0, prev_bytes = 0;
	const char *why = NULL;

	while ((opt = getopt(argc, argv, "p:r:f:g:t:o:b:Tzqh")) != -1) {
		switch (opt) {
		case 'p': port = optarg; break;
		case 'r': rate_req = atof(optarg); has_rate = true; break;
		case 'f': freq = atof(optarg); has_freq = true; break;
		case 'g': gain = strcmp(optarg, "auto") == 0 ? -1.0 : atof(optarg); has_gain = true; break;
		case 't': secs = atof(optarg); break;
		case 'o': outname = optarg; break;
		case 'b': ring_mib = (size_t)atol(optarg); break;
		case 'T': takeover = true; break;
		case 'z': zero_csum = true; break;
		case 'q': quiet = true; break;
		default: usage(); return 2;
		}
	}
	if (optind >= argc) {
		usage();
		return 2;
	}
	host = argv[optind];
	settings_wanted = has_freq || has_gain || has_rate;
	want_rate = has_rate ? rate_req : 0;
	want_freq = freq;
	want_freq_set = has_freq;
	want_gain = gain;
	want_gain_set = has_gain;
	sock = connect_first(host, port);
	if (sock < 0) {
		return 2;
	}
	/* as much receive buffer as the system grants: 128 MiB down to the default's limit */
	for (int mb = 128; mb >= 1; mb /= 2) {
		int sz = mb << 20;

		if (setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) == 0 ||
		    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0) {
			break;
		}
	}
	getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcv, &optlen);
	setsockopt(sock, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
	if (zero_csum) {
		/* IPv6: accept a zero UDP checksum from the board (SOL_UDP 17, UDP_NO_CHECK6_RX 102) */
		if (setsockopt(sock, 17, 102, &one, sizeof(one)) != 0) {
			eprint("vrt_udp: UDP_NO_CHECK6_RX: %s: not asking for zero checksums\n", strerror(errno));
			zero_csum = false;
		}
	}
	/* the receive thread looks at the stop flag every 100 ms */
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &(struct timeval){0, 100000}, sizeof(struct timeval));
	eprint("vrt_udp: receive buffer %d KiB (net.core.rmem_max limits it without root)\n", rcv / 1024);
	if (outname != NULL) {
		if (strcmp(outname, "-") == 0) {
			out_fd = STDOUT_FILENO;
			out_stdout = true;
			fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL) | O_NONBLOCK);
		} else {
			out_fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (out_fd < 0) {
				eprint("vrt_udp: %s: %s\n", outname, strerror(errno));
				return 2;
			}
		}
	}
	ring_n = ring_mib * 1024 * 1024 / sizeof(struct slot);
	ring = calloc(ring_n, sizeof(struct slot));
	if (ring == NULL) {
		eprint("vrt_udp: no memory for the ring\n");
		return 2;
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	pthread_create(&wt, NULL, writer, NULL);
	pthread_create(&rt, NULL, receiver, NULL);

	t0 = tprev = tsub = now_s();
	atomic_store(&last_pkt_us, (int64_t)(t0 * 1000));
	send_sub(takeover);
	while (!atomic_load(&stop)) {
		double t;
		bool cnt = atomic_load(&counting);

		nanosleep(&(struct timespec){0, 100000000}, NULL);
		t = now_s();
		if (atomic_load(&busy)) {
			why = "another host has the stream (VRTB); -T takes it over";
			rc = 3;
			break;
		}
		if (atomic_load(&refused)) {
			why = "the board refused a setting";
			rc = 4;
			break;
		}
		/*
		 * The cookie, renewed with it; a board that forgot us answers with a new one. Often, as a
		 * board under a flood loses most of what it receives and stops the stream after 5 s.
		 */
		if (t - tsub >= RENEW_S) {
			tsub = t;
			send_sub(takeover && !atomic_load(&subscribed));
		}
		if (t - tprev >= 1.0) {
			uint64_t p = atomic_load(&st.packets), s = atomic_load(&st.samples),
				 b = atomic_load(&st.bytes);
			double dt = t - tprev;

			/* both acknowledges: the second one, with the epoch, can be lost on its own */
			if (settings_wanted && atomic_load(&subscribed) &&
			    (!atomic_load(&acked) ||
			     (atomic_load(&ctx_has_epoch) && !atomic_load(&have_ack_epoch))) &&
			    t - tlast_cmd >= 1.0) {
				if (++tries > 5) {
					why = "no acknowledge of the settings";
					rc = 4;
					break;
				}
				send_cmd(freq, has_freq, gain, has_gain, rate_req, has_rate);
				tlast_cmd = t;
			}
			if (!quiet && cnt) {
				eprint("vrt_udp: %.2f MS/s, %.0f packets/s, %.1f Mb/s, lost %llu packets (%llu samples), "
				       "board dropped %llu samples, kernel dropped %llu, overrange %llu, late %llu%s\n",
				       (double)(s - prev_samples) / dt / 1e6, (double)(p - prev_packets) / dt,
				       (double)(b - prev_bytes) * 8.0 / dt / 1e6,
				       (unsigned long long)atomic_load(&st.pkts_lost),
				       (unsigned long long)atomic_load(&st.net_loss_samples),
				       (unsigned long long)atomic_load(&st.board_loss_samples),
				       (unsigned long long)atomic_load(&st.kernel_drops),
				       (unsigned long long)atomic_load(&st.overrange),
				       (unsigned long long)atomic_load(&st.late),
				       atomic_load(&st.writer_drops) ? ", WRITER BEHIND" : "");
			}
			prev_packets = p;
			prev_samples = s;
			prev_bytes = b;
			tprev = t;
		}
		if (cnt && (int64_t)(t * 1000) - atomic_load(&last_pkt_us) > 5000) {
			atomic_store(&silent, true);
			why = "the board has been silent for 5 s";
			break;
		}
		if (cnt && secs > 0 && (int64_t)(t * 1000) >= atomic_load(&counting_ms) + (int64_t)(secs * 1000)) {
			break;
		}
		if (!cnt && t - t0 > (secs > 0 ? secs : 0) + 15.0) {
			static char msg[160];

			snprintf(msg, sizeof(msg), "no stream within 15 s: cookie %d, subscribed %d, acked %d, rate %.0f, context ok %d",
				 (int)atomic_load(&have_cookie), (int)atomic_load(&subscribed), (int)atomic_load(&acked),
				 atomic_load(&rate_a), (int)atomic_load(&ctx_ok));
			why = msg;
			rc = 5;
			break;
		}
	}
	/* stop: the board is told, the threads finish what they hold; whatever happens, 5 s */
	alarm(5);
	if (atomic_load(&have_cookie)) {
		uint8_t m[12];
		uint64_t ck = atomic_load(&cookie_a);

		memcpy(m, "VRTX", 4);
		memcpy(m + 4, &ck, 8);
		(void)send(sock, m, 12, 0);
	}
	atomic_store(&stop, true);
	pthread_join(rt, NULL);
	pthread_join(wt, NULL);
	if (out_fd >= 0 && out_fd != STDOUT_FILENO && close(out_fd) != 0) {
		atomic_fetch_add(&st.write_err, 1);
	}
	{
		uint64_t pk = atomic_load(&st.packets);
		double dur = secs;

		eprint("vrt_udp: %llu packets, %llu samples; lost on the network: %llu packets / %llu samples; "
		       "dropped by the board: %llu packets / %llu samples; restarts: %llu; kernel drops: %llu; "
		       "writer: %llu, write errors %llu; bad: %llu; zeros written: %llu; overrange packets: %llu\n",
		       (unsigned long long)pk, (unsigned long long)atomic_load(&st.samples),
		       (unsigned long long)atomic_load(&st.pkts_lost),
		       (unsigned long long)atomic_load(&st.net_loss_samples),
		       (unsigned long long)atomic_load(&st.board_loss_pkts),
		       (unsigned long long)atomic_load(&st.board_loss_samples),
		       (unsigned long long)atomic_load(&st.resyncs),
		       (unsigned long long)atomic_load(&st.kernel_drops),
		       (unsigned long long)atomic_load(&st.writer_drops),
		       (unsigned long long)atomic_load(&st.write_err),
		       (unsigned long long)atomic_load(&st.bad),
		       (unsigned long long)atomic_load(&st.filled),
		       (unsigned long long)atomic_load(&st.overrange));
		if (why != NULL) {
			eprint("vrt_udp: %s\n", why);
		}
		if (rc == 0 && (pk == 0 || atomic_load(&st.pkts_lost) || atomic_load(&st.net_loss_samples) ||
				atomic_load(&st.board_loss_samples) || atomic_load(&st.resyncs) ||
				atomic_load(&st.kernel_drops) || atomic_load(&st.writer_drops) ||
				atomic_load(&st.write_err) || atomic_load(&st.bad) || atomic_load(&silent))) {
			rc = 1;
		}
		if (rc == 0 && dur > 0 && (double)atomic_load(&st.samples) < 0.9 * atomic_load(&rate_a) * dur) {
			eprint("vrt_udp: short capture: %llu samples for %.1f s\n",
			       (unsigned long long)atomic_load(&st.samples), dur);
			rc = 1;
		}
	}
	(void)out_stdout;
	return rc;
}
