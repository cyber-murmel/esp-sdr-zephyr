// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * udp_tx: UDP packets with a sequence number and a pattern (src/rxb_proto.h) to one unicast host,
 * paced with absolute deadlines, in batches with sendmmsg(). For apps/rxbench: the board counts
 * what arrives. See ../README.rst.
 *
 *   gcc -O2 -Wall -Wextra -pthread -o udp_tx udp_tx.c -lm
 *   ./udp_tx -r 300 -t 10 fe80::1234%enp0s20f0u1u3
 *   ./udp_tx --selftest
 *
 * Linux, gcc.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/rxb_proto.h"
#include "../src/rxb_seq.h"

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef SOL_UDP
#define SOL_UDP 17
#endif

#define NS_PER_S 1000000000ULL
#define DEF_PAYLOAD_V6 1420U
#define DEF_PAYLOAD_V4 1440U
/* Largest payloads that fit a 1500 byte MTU without fragmenting (the board takes no fragments) */
#define MAX_PAYLOAD_V6 1452U
#define MAX_PAYLOAD_V4 1472U
#define MAX_RATE_MBPS 1000.0
#define MAX_BATCH 256U
/* Aim at this much time per batch: shorter means more wakeups, longer means burstier */
#define BATCH_TARGET_NS 100000ULL
/* Sleep until this long before a deadline, spin for the rest */
#define SPIN_NS 60000ULL
/* Behind by more than this: skip the lost time instead of catching up with a burst */
#define RESYNC_NS (100ULL * 1000000ULL)
#define MAX_GSO 64U
/* A message of GSO segments must fit one UDP datagram */
#define MAX_DGRAM 65000U
#define MAX_SECONDS 86400.0
#define SNDBUF_BYTES (4 * 1024 * 1024)
#define RCVBUF_BYTES (8 * 1024 * 1024)
/* Ethernet header, FCS, preamble and gap; IP and UDP headers on top */
#define WIRE_OVERHEAD 38U
#define IP4_UDP_HDRS 28U
#define IP6_UDP_HDRS 48U

struct cfg {
	struct sockaddr_storage dst;
	socklen_t dstlen;
	unsigned int payload;	/* bytes of UDP payload per packet */
	double pps;		/* packets per second */
	double seconds;
	unsigned int gso;	/* segments per message, 1: no GSO */
	bool quiet;
};

struct result {
	uint64_t packets, messages, calls;
	uint64_t eagain, refused, resyncs;
	uint64_t max_late_ns;
	double elapsed_s;
	unsigned int payload;
	unsigned int hdrs;
	uint32_t first_seq, next_seq;
};

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig)
{
	(void)sig;
	stop_flag = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t_ns)
{
	struct timespec ts = {.tv_sec = (time_t)(t_ns / NS_PER_S), .tv_nsec = (long)(t_ns % NS_PER_S)};

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !stop_flag) {
	}
}

/* Multicast, broadcast and the unspecified address: never a target of this tool. */
static bool refused_address(const struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET) {
		uint32_t a = ntohl(((const struct sockaddr_in *)ss)->sin_addr.s_addr);

		return (a >> 28) == 0xe || a == 0xffffffffU || a == 0U;
	}
	if (ss->ss_family == AF_INET6) {
		const struct in6_addr *a = &((const struct sockaddr_in6 *)ss)->sin6_addr;

		/* ::ffff:a.b.c.d goes out as IPv4 on Linux: refuse them all, so that no multicast or
		 * broadcast address gets through in that form
		 */
		return a->s6_addr[0] == 0xff || IN6_IS_ADDR_UNSPECIFIED(a) || IN6_IS_ADDR_V4MAPPED(a);
	}
	return true;
}

/* family 0: either, IPv6 preferred */
static int resolve(const char *host, int family, uint16_t port, struct sockaddr_storage *out,
		   socklen_t *outlen)
{
	struct addrinfo hints = {.ai_family = family, .ai_socktype = SOCK_DGRAM};
	struct addrinfo *res, *p, *pick = NULL;
	int rc = getaddrinfo(host, NULL, &hints, &res);

	if (rc != 0) {
		fprintf(stderr, "udp_tx: %s: %s\n", host, gai_strerror(rc));
		return -1;
	}
	/* the first IPv6 result, else the first IPv4 one */
	for (p = res; p != NULL && (pick == NULL || pick->ai_family != AF_INET6);
	     p = p->ai_next) {
		if (p->ai_family == AF_INET6 || (p->ai_family == AF_INET && pick == NULL)) {
			pick = p;
		}
	}
	if (pick == NULL) {
		fprintf(stderr, "udp_tx: %s: no usable address\n", host);
		freeaddrinfo(res);
		return -1;
	}
	memset(out, 0, sizeof(*out));
	memcpy(out, pick->ai_addr, pick->ai_addrlen);
	*outlen = pick->ai_addrlen;
	if (out->ss_family == AF_INET6) {
		((struct sockaddr_in6 *)out)->sin6_port = htons(port);
	} else {
		((struct sockaddr_in *)out)->sin_port = htons(port);
	}
	freeaddrinfo(res);
	return 0;
}

static void addr_str(const struct sockaddr_storage *ss, char *s, size_t n)
{
	if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
		char ip[INET6_ADDRSTRLEN];

		inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
		if (a->sin6_scope_id != 0) {
			snprintf(s, n, "[%s%%%u]:%u", ip, a->sin6_scope_id, ntohs(a->sin6_port));
		} else {
			snprintf(s, n, "[%s]:%u", ip, ntohs(a->sin6_port));
		}
	} else {
		const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
		char ip[INET_ADDRSTRLEN];

		inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
		snprintf(s, n, "%s:%u", ip, ntohs(a->sin_port));
	}
}

struct batch {
	struct mmsghdr *msgs;
	struct iovec *iov;
	uint8_t **buf;
	char (*ctl)[CMSG_SPACE(sizeof(uint16_t))];
	unsigned int nmsg;	/* messages per batch */
};

static int batch_init(struct batch *b, const struct cfg *c, unsigned int npkts)
{
	size_t msg_bytes = (size_t)c->payload * c->gso;

	b->nmsg = npkts / c->gso;
	b->msgs = calloc(b->nmsg, sizeof(*b->msgs));
	b->iov = calloc(b->nmsg, sizeof(*b->iov));
	b->buf = calloc(b->nmsg, sizeof(*b->buf));
	b->ctl = calloc(b->nmsg, sizeof(*b->ctl));
	if (b->msgs == NULL || b->iov == NULL || b->buf == NULL || b->ctl == NULL) {
		return -1;
	}
	for (unsigned int m = 0; m < b->nmsg; m++) {
		b->buf[m] = malloc(msg_bytes);
		if (b->buf[m] == NULL) {
			return -1;
		}
		/* the pattern once; only the numbers change per packet */
		for (unsigned int g = 0; g < c->gso; g++) {
			rxb_fill(b->buf[m] + (size_t)g * c->payload, c->payload, 0);
		}
		b->iov[m].iov_base = b->buf[m];
		b->iov[m].iov_len = msg_bytes;
		b->msgs[m].msg_hdr.msg_iov = &b->iov[m];
		b->msgs[m].msg_hdr.msg_iovlen = 1;
		if (c->gso > 1) {
			struct msghdr *h = &b->msgs[m].msg_hdr;
			struct cmsghdr *cm;
			uint16_t seg = (uint16_t)c->payload;

			h->msg_control = b->ctl[m];
			h->msg_controllen = sizeof(b->ctl[m]);
			cm = CMSG_FIRSTHDR(h);
			cm->cmsg_level = SOL_UDP;
			cm->cmsg_type = UDP_SEGMENT;
			cm->cmsg_len = CMSG_LEN(sizeof(seg));
			memcpy(CMSG_DATA(cm), &seg, sizeof(seg));
		}
	}
	return 0;
}

static void batch_free(struct batch *b)
{
	for (unsigned int m = 0; b->buf != NULL && m < b->nmsg; m++) {
		free(b->buf[m]);
	}
	free(b->msgs);
	free(b->iov);
	free(b->buf);
	free(b->ctl);
}

static void put_seq(uint8_t *p, uint32_t seq)
{
	p[0] = (uint8_t)(seq >> 24);
	p[1] = (uint8_t)(seq >> 16);
	p[2] = (uint8_t)(seq >> 8);
	p[3] = (uint8_t)seq;
}

static int run_send(const struct cfg *c, struct result *r)
{
	unsigned int hdrs = c->dst.ss_family == AF_INET6 ? IP6_UDP_HDRS : IP4_UDP_HDRS;
	unsigned int npkts, nbatch_msgs;
	double interval_ns;
	struct batch b = {0};
	uint64_t t0, t_start, t_end, k = 0, t_next;
	uint32_t seq = 0;
	int fd, sndbuf = SNDBUF_BYTES, rc = -1;

	memset(r, 0, sizeof(*r));
	r->payload = c->payload;
	r->hdrs = hdrs;
	/* packets per batch: about BATCH_TARGET_NS of traffic, whole GSO messages */
	npkts = (unsigned int)ceil(c->pps * (double)BATCH_TARGET_NS / (double)NS_PER_S);
	npkts = ((npkts + c->gso - 1U) / c->gso) * c->gso;
	if (npkts < c->gso) {
		npkts = c->gso;
	}
	if (npkts > MAX_BATCH) {
		npkts = MAX_BATCH / c->gso * c->gso;
	}
	nbatch_msgs = npkts / c->gso;
	interval_ns = (double)npkts * (double)NS_PER_S / c->pps;

	fd = socket(c->dst.ss_family, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	/* no SO_BROADCAST: the kernel refuses a directed broadcast on its own */
	if (connect(fd, (const struct sockaddr *)&c->dst, c->dstlen) != 0) {
		perror("connect");
		goto out;
	}
	if (batch_init(&b, c, npkts) != 0) {
		fprintf(stderr, "udp_tx: out of memory\n");
		goto out;
	}

	t0 = now_ns() + 2000000ULL;
	t_start = t0;
	t_end = t0 + (uint64_t)(c->seconds * (double)NS_PER_S);
	r->first_seq = seq;
	while (!stop_flag) {
		unsigned int off = 0;
		uint64_t now;

		t_next = t0 + (uint64_t)((double)k * interval_ns);
		if (t_next >= t_end) {
			break;
		}
		now = now_ns();
		if (t_next > now) {
			if (t_next - now > SPIN_NS) {
				sleep_until(t_next - SPIN_NS);
			}
			while (now_ns() < t_next && !stop_flag) {
			}
		} else {
			if (now - t_next > r->max_late_ns) {
				r->max_late_ns = now - t_next;
			}
			if (now - t_next > RESYNC_NS) {
				/* a stall: do not make up for it with a burst. The report keeps
				 * the real start, so the stall lowers the rate it shows.
				 */
				t0 += now - t_next;
				r->resyncs++;
				t_next = now;
			}
		}
		if (stop_flag) {
			break;
		}
		for (unsigned int m = 0; m < nbatch_msgs; m++) {
			for (unsigned int g = 0; g < c->gso; g++) {
				put_seq(b.buf[m] + (size_t)g * c->payload, seq++);
			}
		}
		while (off < nbatch_msgs) {
			int n = sendmmsg(fd, b.msgs + off, nbatch_msgs - off, 0);

			r->calls++;
			if (n < 0) {
				if (errno == EINTR) {
					if (stop_flag) {
						break;
					}
					continue;
				}
				if (errno == EAGAIN || errno == ENOBUFS || errno == ECONNREFUSED) {
					if (errno == ECONNREFUSED) {
						/* an ICMP error of an earlier packet: report, go on */
						r->refused++;
					} else {
						r->eagain++;
					}
					if (stop_flag || now_ns() >= t_end) {
						break;
					}
					continue;
				}
				perror("sendmmsg");
				goto out;
			}
			off += (unsigned int)n;
		}
		/* what the kernel took; a stop in the middle leaves the numbers of the rest unsent */
		r->messages += off;
		r->packets += (uint64_t)off * c->gso;
		seq = r->first_seq + (uint32_t)r->packets;
		k++;
	}
	r->elapsed_s = (double)(now_ns() - t_start) / (double)NS_PER_S;
	if (r->elapsed_s <= 0.0) {
		r->elapsed_s = 1e-9;
	}
	r->next_seq = seq;
	rc = 0;
out:
	batch_free(&b);
	close(fd);
	return rc;
}

static void report(const struct cfg *c, const struct result *r)
{
	double pps = (double)r->packets / r->elapsed_s;
	double pay = pps * r->payload * 8.0 / 1e6;
	double wire = pps * (r->payload + r->hdrs + WIRE_OVERHEAD) * 8.0 / 1e6;

	printf("sent %" PRIu64 " packets of %u bytes in %.3f s: %.0f packets/s, %.1f Mb/s UDP payload, "
	       "%.1f Mb/s on the wire (asked %.0f packets/s)\n",
	       r->packets, r->payload, r->elapsed_s, pps, pay, wire, c->pps);
	printf("sequence %u to %u; %" PRIu64 " sendmmsg calls (%" PRIu64 " messages%s), "
	       "latest start of a batch %.0f us late, %" PRIu64 " resyncs after a stall, "
	       "%" PRIu64 " buffer-full retries, %" PRIu64 " refused (ICMP)\n",
	       r->first_seq, r->next_seq, r->calls, r->messages,
	       c->gso > 1 ? ", GSO" : "", (double)r->max_late_ns / 1e3, r->resyncs, r->eagain,
	       r->refused);
}

/* ---- loopback self-test ---- */

struct sink {
	int fd;
	volatile bool stop;
	uint64_t packets, bad;
	struct rxb_seq seq;
	struct rxb_seq_counts counts;
};

static void *sink_thread(void *arg)
{
	struct sink *s = arg;
	enum { N = 32, BUFSZ = 2048 };
	static uint8_t bufs[N][BUFSZ];
	struct mmsghdr m[N];
	struct iovec iov[N];

	for (int i = 0; i < N; i++) {
		iov[i].iov_base = bufs[i];
		iov[i].iov_len = BUFSZ;
		memset(&m[i], 0, sizeof(m[i]));
		m[i].msg_hdr.msg_iov = &iov[i];
		m[i].msg_hdr.msg_iovlen = 1;
	}
	while (!s->stop) {
		int n = recvmmsg(s->fd, m, N, 0, NULL);

		for (int i = 0; i < n; i++) {
			uint32_t q;

			s->packets++;
			if (rxb_check(bufs[i], m[i].msg_len, true, &q)) {
				rxb_seq_track(&s->seq, q, &s->counts);
			} else {
				s->bad++;
			}
		}
	}
	return NULL;
}

static int selftest_one(const char *host, unsigned int payload, double mbps, unsigned int gso)
{
	struct sink s = {0};
	struct cfg c = {.payload = payload, .seconds = 1.0, .gso = gso, .quiet = true};
	struct sockaddr_storage rx;
	struct result r;
	struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
	socklen_t len;
	pthread_t th;
	int rcvbuf = RCVBUF_BYTES, fails = 0;
	char a[96];

	if (resolve(host, 0, 0, &rx, &len) != 0) {
		return 1;
	}
	s.fd = socket(rx.ss_family, SOCK_DGRAM, 0);
	setsockopt(s.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (bind(s.fd, (struct sockaddr *)&rx, len) != 0) {
		perror("selftest bind");
		return 1;
	}
	len = sizeof(rx);
	getsockname(s.fd, (struct sockaddr *)&rx, &len);
	c.dst = rx;
	c.dstlen = len;
	c.pps = mbps * 1e6 / 8.0 / payload;
	pthread_create(&th, NULL, sink_thread, &s);
	if (run_send(&c, &r) != 0) {
		s.stop = true;
		pthread_join(th, NULL);
		close(s.fd);
		return 1;
	}
	usleep(300000);
	s.stop = true;
	pthread_join(th, NULL);
	close(s.fd);
	addr_str(&rx, a, sizeof(a));
	printf("self-test %s %u B x%u at %.0f Mb/s: sent %" PRIu64 ", received %" PRIu64
	       ", bad %" PRIu64 ", lost %u, dup %u, ooo %u, %.0f packets/s (asked %.0f)\n",
	       a, payload, gso, mbps, r.packets, s.packets, s.bad, s.counts.lost, s.counts.dups,
	       s.counts.ooo, (double)r.packets / r.elapsed_s, c.pps);
	if (s.bad != 0 || s.counts.dups != 0 || s.counts.ooo != 0) {
		fprintf(stderr, "  FAIL: corrupt, duplicated or reordered packets\n");
		fails++;
	}
	if (s.packets + s.packets / 100 < r.packets || s.packets > r.packets) {
		fprintf(stderr, "  FAIL: received %" PRIu64 " of %" PRIu64 " sent\n", s.packets,
			r.packets);
		fails++;
	}
	if (fabs((double)r.packets / r.elapsed_s - c.pps) > c.pps * 0.03) {
		fprintf(stderr, "  FAIL: the rate is off by more than 3 %%\n");
		fails++;
	}
	return fails;
}

static int selftest(void)
{
	static const char *const bad[] = {"224.0.0.1", "239.255.0.1", "255.255.255.255", "0.0.0.0",
					  "ff02::1", "ff0e::1234", "::", "::ffff:239.1.1.1", "::ffff:0.0.0.0",
					  "::ffff:255.255.255.255", "::ffff:10.0.0.1"};
	struct sockaddr_storage ss;
	socklen_t len;
	int fails = 0;

	for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (resolve(bad[i], 0, RXB_PORT, &ss, &len) != 0 || !refused_address(&ss)) {
			fprintf(stderr, "FAIL: %s is not refused\n", bad[i]);
			fails++;
		}
	}
	printf("self-test: multicast, broadcast and unspecified addresses are refused\n");
	fails += selftest_one("::1", DEF_PAYLOAD_V6, 160.0, 1);
	fails += selftest_one("127.0.0.1", DEF_PAYLOAD_V4, 160.0, 1);
	fails += selftest_one("127.0.0.1", 100, 20.0, 1);
	fails += selftest_one("::1", DEF_PAYLOAD_V6, 160.0, 4);
	printf(fails == 0 ? "self-test: PASS\n" : "self-test: FAIL\n");
	return fails != 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: udp_tx (-r Mb/s | -p packets/s) [-t seconds] [-s payload] [-P port]\n"
		"              [-4 | -6] [-G segments] HOST\n"
		"       udp_tx --selftest\n"
		"  HOST   one unicast host (name, IPv4, or IPv6 with %%interface for link-local);\n"
		"         multicast and broadcast are refused. IPv6 is preferred for a name with both.\n"
		"  -r     rate in Mb/s of UDP payload (what goes on the wire, headers and gaps counted, may be\n         up to %.0f); -p: packets per second instead\n"
		"  -t     seconds to send, then stop (default 10)\n"
		"  -s     UDP payload bytes (default %u for IPv6, %u for IPv4; %u..%u/%u)\n"
		"  -P     UDP port (default %u, the one port the board takes)\n"
		"  -G     UDP GSO: this many packets per message (2..%u), fewer system calls\n",
		MAX_RATE_MBPS, DEF_PAYLOAD_V6, DEF_PAYLOAD_V4, RXB_MIN_PAYLOAD, MAX_PAYLOAD_V6,
		MAX_PAYLOAD_V4, RXB_PORT, MAX_GSO);
}

int main(int argc, char **argv)
{
	static const struct option lopts[] = {{"selftest", no_argument, NULL, 'T'}, {0}};
	struct cfg c = {.seconds = 10.0, .gso = 1};
	double mbps = 0.0;
	unsigned int payload = 0;
	unsigned long port = RXB_PORT;
	int family = 0, opt;
	struct sigaction sa = {.sa_handler = on_signal};
	struct result r;
	char a[96];

	while ((opt = getopt_long(argc, argv, "r:p:t:s:P:46G:h", lopts, NULL)) != -1) {
		switch (opt) {
		case 'T':
			return selftest();
		case 'r':
			mbps = atof(optarg);
			break;
		case 'p':
			c.pps = atof(optarg);
			break;
		case 't':
			c.seconds = atof(optarg);
			break;
		case 's':
			payload = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'P':
			port = strtoul(optarg, NULL, 10);
			break;
		case '4':
			family = AF_INET;
			break;
		case '6':
			family = AF_INET6;
			break;
		case 'G':
			c.gso = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind + 1 != argc || (mbps > 0.0) == (c.pps > 0.0)) {
		usage();
		return 2;
	}
	if (port == 0 || port > 65535 || !isfinite(c.seconds) || c.seconds <= 0.0 ||
	    c.seconds > MAX_SECONDS || !isfinite(mbps) || !isfinite(c.pps) || mbps < 0.0 ||
	    c.pps < 0.0 || c.gso < 1 || c.gso > MAX_GSO) {
		usage();
		return 2;
	}
	if (resolve(argv[optind], family, (uint16_t)port, &c.dst, &c.dstlen) != 0) {
		return 1;
	}
	if (refused_address(&c.dst)) {
		fprintf(stderr, "udp_tx: %s is a multicast, broadcast, unspecified or IPv4-mapped address: "
				"unicast hosts only\n", argv[optind]);
		return 1;
	}
	if (payload == 0) {
		payload = c.dst.ss_family == AF_INET6 ? DEF_PAYLOAD_V6 : DEF_PAYLOAD_V4;
	}
	if (payload < RXB_MIN_PAYLOAD ||
	    payload > (c.dst.ss_family == AF_INET6 ? MAX_PAYLOAD_V6 : MAX_PAYLOAD_V4)) {
		fprintf(stderr, "udp_tx: payload %u: from %u bytes up to what a 1500 byte MTU takes\n",
			payload, RXB_MIN_PAYLOAD);
		return 2;
	}
	c.payload = payload;
	if (mbps > 0.0) {
		c.pps = mbps * 1e6 / 8.0 / payload;
	}
	if ((size_t)payload * c.gso > MAX_DGRAM) {
		fprintf(stderr, "udp_tx: %u segments of %u bytes do not fit one datagram\n", c.gso,
			payload);
		return 2;
	}
	if (c.pps * (payload + (c.dst.ss_family == AF_INET6 ? IP6_UDP_HDRS : IP4_UDP_HDRS) +
		     WIRE_OVERHEAD) * 8.0 / 1e6 > MAX_RATE_MBPS) {
		fprintf(stderr, "udp_tx: more than %.0f Mb/s on the wire is more than the link\n",
			MAX_RATE_MBPS);
		return 2;
	}
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	addr_str(&c.dst, a, sizeof(a));
	printf("sending to %s: %u byte payload, %.0f packets/s (%.1f Mb/s), %.1f s%s\n", a, payload,
	       c.pps, c.pps * payload * 8.0 / 1e6, c.seconds, c.gso > 1 ? ", GSO" : "");
	if (run_send(&c, &r) != 0) {
		return 1;
	}
	report(&c, &r);
	return 0;
}
