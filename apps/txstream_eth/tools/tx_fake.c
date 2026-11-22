// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * tx_fake: a stand-in for the board's transmit stream, to test vrt_tx on the loopback (127.0.0.1
 * and ::1 only; nothing leaves the machine). It speaks the cookie handshake of vrt_fake, the
 * messages of tx_proto.h, and plays the board model of tx_board.h in real time: a status every
 * 250 us, credits, underruns. It spins while a run goes on (one core at 100 %): sleeping would make
 * its flips and statuses late by the scheduler's slack, which the real board's CPU1 thread is not. Data packets are parsed with the board's own parser
 * (lib/esp_sdr/src/txs/txs_wire.c) and, with -T, compared with the tone the sender should make.
 *
 *   tx_fake [-p ctl port] [-d data port] [-T kHz:amp] [-L N] [-S at_s:ms] [-R N] [-t seconds] [-q]
 *     -T  check the payload against tone:kHz:amp (counts the wrong packets)
 *     -L  drop every Nth data packet (loss on the network)
 *     -S  stall the board's CPU for ms milliseconds at_s seconds after the run starts
 *     -R  refuse the first N TXGOs with code 5 (RF not armed), then accept the next, RF included (the
 *         board after `tx arm`); without -R an RF run is always refused
 *     -D  the start of a run takes ms milliseconds, during which the control thread answers nothing
 *     -t  give up after this many seconds (default 60)
 * Exit 0 when every packet was well formed and right, and the board never underran.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "tx_board.h"
#include "tx_hold.h"
#include "tx_proto.h"
#include "tx_src.h"
#include "txs_wire.h"

static int64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint64_t secret = 0x1234abcd5678ef01ULL;

static void cookie_of(const struct sockaddr_storage *a, uint8_t out[8])
{
	uint64_t h = secret;
	const uint8_t *p = (const uint8_t *)a;

	/* the address and port only (not the padding): fine for a fake */
	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *s = (const void *)a;

		h ^= ((uint64_t)s->sin_addr.s_addr << 16) ^ s->sin_port;
	} else {
		const struct sockaddr_in6 *s = (const void *)a;

		for (int i = 0; i < 16; i++) {
			h = h * 1099511628211ULL + s->sin6_addr.s6_addr[i];
		}
		h ^= s->sin6_port;
	}
	(void)p;
	h *= 0x9e3779b97f4a7c15ULL;
	txp_put64(out, h);
}

static int bind_udp(int family, int port)
{
	int fd = socket(family, SOCK_DGRAM, 0), on = 1, big = 1 << 22;

	if (fd < 0) {
		return -1;
	}
	if (family == AF_INET6) {
		struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port),
					 .sin6_addr = IN6ADDR_LOOPBACK_INIT};

		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
		return bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 ? fd : -1;
	} else {
		struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
					.sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
		return bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 ? fd : -1;
	}
}

static bool same(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family) {
		return false;
	}
	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;

		return x->sin_addr.s_addr == y->sin_addr.s_addr && x->sin_port == y->sin_port;
	}
	return memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr, &((const struct sockaddr_in6 *)b)->sin6_addr, 16) == 0 &&
	       ((const struct sockaddr_in6 *)a)->sin6_port == ((const struct sockaddr_in6 *)b)->sin6_port;
}

int main(int argc, char **argv)
{
	int cport = 4992, dport = 4994, opt, drop_every = 0, refuse_n = 0, refused = 0;
	double start_ms = 0;
	double tone_khz = 0, secs = 60, stall_at = -1, stall_ms = 0;
	int tone_amp = 0;
	bool quiet = false;
	int cs[2], ds[2], ncs = 0;
	struct pollfd pf[4];
	struct sockaddr_storage sub;
	socklen_t sublen = 0;
	bool subscribed = false, have_run = false, ended = false, stalled = false;
	struct tbm b;
	struct txs_wire_cfg wc = {.port = 0};
	struct txsrc_tone tone;
	uint64_t pkts = 0, words = 0, bad_hdr = 0, wrong = 0, dropped = 0, nstat = 0;
	int64_t t_run = 0, next_status = 0, t_end;
	uint32_t seq_n = 0, rate_msps = 20, fmt = TXP_FMT_IQ8;
	uint8_t buf[2048];
	static uint8_t want[2048];

	while ((opt = getopt(argc, argv, "p:d:T:L:S:R:D:t:q")) != -1) {
		switch (opt) {
		case 'p': cport = atoi(optarg); break;
		case 'd': dport = atoi(optarg); break;
		case 'T': {
			char *e;

			tone_khz = strtod(optarg, &e);
			tone_amp = *e == ':' ? atoi(e + 1) : 1;
			break;
		}
		case 'L': drop_every = atoi(optarg); break;
		case 'S': {
			char *e;

			stall_at = strtod(optarg, &e);
			stall_ms = *e == ':' ? atof(e + 1) : 5;
			break;
		}
		case 'D': start_ms = atof(optarg); break;
		case 'R': refuse_n = atoi(optarg); break;
		case 't': secs = atof(optarg); break;
		case 'q': quiet = true; break;
		default: return 2;
		}
	}
	for (int f = 0; f < 2; f++) {
		int fam = f == 0 ? AF_INET : AF_INET6;
		int c = bind_udp(fam, cport), d = bind_udp(fam, dport);

		if (c >= 0 && d >= 0) {
			cs[ncs] = c;
			ds[ncs] = d;
			ncs++;
		}
	}
	if (ncs == 0) {
		fprintf(stderr, "tx_fake: cannot bind %d/%d\n", cport, dport);
		return 2;
	}
	t_end = now_ns() + (int64_t)(secs * 1e9);
	fprintf(stderr, "tx_fake: listening on %d (control) and %d (data)\n", cport, dport);
	tbm_init(&b, rate_msps, 32768, TX_DEPTH_PKTS * 702U, 3510, 2000000);

	while (now_ns() < t_end) {
		int64_t now = now_ns(), dl = next_status;
		int ms = 1;

		if (have_run && !ended && subscribed && b.run) {
			int64_t d = tbm_deadline(&b);

			dl = d < dl ? d : dl;
		}
		if (dl - now > 0) {
			ms = (int)((dl - now) / 1000000);
		} else {
			ms = 0;
		}
		for (int i = 0; i < ncs; i++) {
			pf[2 * i] = (struct pollfd){.fd = cs[i], .events = POLLIN};
			pf[2 * i + 1] = (struct pollfd){.fd = ds[i], .events = POLLIN};
		}
		if (ms > 0 && !(subscribed && have_run)) {
			ms = ms > 20 ? 20 : ms;
		}
		if (poll(pf, (nfds_t)(2 * ncs), subscribed && have_run ? 0 : ms) < 0) {
			continue;
		}
		now = now_ns();
		if (stall_at >= 0 && have_run && !stalled && now - t_run > (int64_t)(stall_at * 1e9)) {
			stalled = true;
			fprintf(stderr, "tx_fake: board CPU stalls for %.1f ms\n", stall_ms);
			nanosleep(&(struct timespec){0, (long)(stall_ms * 1e6)}, NULL);
			now = now_ns();
		}
		if (have_run && !ended) {
			tbm_advance(&b, now);
		}
		for (int i = 0; i < ncs; i++) {
			for (int k = 0; k < 2; k++) {
				if (!(pf[2 * i + k].revents & POLLIN) && subscribed && have_run && k == 1) {
					/* fall through: poll with timeout 0 may have missed it, try the read anyway */
				}
				for (int n = 0; n < 256; n++) {
					struct sockaddr_storage from;
					socklen_t fl = sizeof(from);
					int fd = k == 0 ? cs[i] : ds[i];
					ssize_t len = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
					uint8_t ck[8];

					if (len < 0) {
						break;
					}
					now = now_ns();
					cookie_of(&from, ck);
					if (k == 1) {
						struct txs_wire_pkt pk;
						enum txs_wire_err e;

						if (!subscribed || !have_run || !same(&from, &sub)) {
							bad_hdr++;
							continue;
						}
						e = txs_wire_parse(&wc, (uint16_t)dport, buf, (size_t)len, &pk);
						if (e != TXS_WIRE_OK) {
							bad_hdr++;
							if (bad_hdr < 5) {
								fprintf(stderr, "tx_fake: bad packet (error %d)\n", (int)e);
							}
							continue;
						}
						pkts++;
						if (drop_every > 0 && pkts % (uint64_t)drop_every == 0) {
							dropped++;
							continue;
						}
						if (tone_amp > 0) {
							txsrc_tone_fill(&tone, pk.index, pk.words, want, fmt);
							if (memcmp(want, pk.payload, pk.bytes) != 0) {
								wrong++;
							}
						}
						words += pk.words;
						tbm_advance(&b, now);
						tbm_packet(&b, now, pk.index, pk.words);
						continue;
					}
					if (len == 4 && memcmp(buf, "VRTS", 4) == 0) {
						uint8_t r[12];

						memcpy(r, "VRTC", 4);
						memcpy(r + 4, ck, 8);
						sendto(fd, r, 12, 0, (struct sockaddr *)&from, fl);
					} else if (len == 12 && memcmp(buf, "VRTS", 4) == 0 && memcmp(buf + 4, ck, 8) == 0) {
						if (subscribed && !same(&from, &sub)) {
							sendto(fd, "VRTB", 4, 0, (struct sockaddr *)&from, fl);
						} else if (!subscribed) {
							sub = from;
							sublen = fl;
							subscribed = true;
							have_run = false;
							fprintf(stderr, "tx_fake: subscriber\n");
							sendto(fd, "VRTK", 4, 0, (struct sockaddr *)&from, fl);
						}
					} else if (len == 12 && memcmp(buf, "VRTX", 4) == 0 && subscribed && same(&from, &sub)) {
						subscribed = false;
						have_run = false;
						fprintf(stderr, "tx_fake: stopped by the subscriber\n");
					} else if (subscribed && same(&from, &sub)) {
						char kind = txp_kind(buf, (size_t)len);

						if (kind == 'G') {
							struct txp_go go;
							struct txp_ak ak = {0};

							txp_go_get(&go, buf, (size_t)len);
							if (memcmp(go.cookie, ck, 8) != 0) {
								continue;
							}
							ak.nonce = go.nonce;
							if (refuse_n > 0 && !have_run && refused < refuse_n) {
								/* the board before `tx arm`: nothing starts, code 5 */
								refused++;
								ak.code = TXP_RF_REFUSED;
								{
									uint8_t m[TXP_AK_BYTES];

									txp_ak_put(m, sizeof(m), &ak);
									sendto(fd, m, sizeof(m), 0, (struct sockaddr *)&from, fl);
								}
								continue;
							}
							ak.code = txp_go_valid(&go);
							if (ak.code == TXP_OK && have_run && !ended) {
								/* a retry of the same TXGO is answered again */
								ak.code = TXP_OK;
							}
							if (ak.code == TXP_OK && !have_run) {
								if (start_ms > 0) {
									/* the start blocks the control thread (RF: chain on) */
									nanosleep(&(struct timespec){0, (long)(start_ms * 1e6)}, NULL);
									now = now_ns();
								}
								rate_msps = go.rate_msps;
								fmt = go.fmt;
								tbm_init(&b, rate_msps, 32768, TX_DEPTH_PKTS * 702U /* the same in words for w32: the fake tests the transport, a w32 depth of 6669 is thin at 40 MS/s on a loaded host */, 3510, 2000000);
								b.session = 0x5d5d1234U ^ go.nonce;
								wc.stream_id = b.session;
								wc.port = (uint16_t)dport;
								wc.fmt = fmt == TXP_FMT_IQ8 ? TXS_FMT_IQ8 : TXS_FMT_WORD32;
								wc.max_words = fmt == TXP_FMT_IQ8 ? 702 : 351;
								if (tone_amp > 0) {
									txsrc_tone_init(&tone, rate_msps * 1000000U,
											(int64_t)(tone_khz * 1000.0), tone_amp);
								}
								have_run = true;
								ended = false;
								t_run = now;
								next_status = now;
								fprintf(stderr, "tx_fake: run: %u MS/s, fmt %u, flags %x\n", rate_msps, fmt, go.flags);
							}
							if ((go.flags & TXP_GO_RF) != 0U && refuse_n == 0) {
								ak.code = TXP_RF_REFUSED;
							}
							ak.flags = (go.flags & TXP_GO_RF) != 0U && ak.code == TXP_OK ? TXP_AK_RF : 0;
							ak.stream_id = b.session;
							ak.max_words = wc.max_words;
							ak.depth = b.depth;
							ak.group = b.group;
							{
								uint8_t m[TXP_AK_BYTES];

								txp_ak_put(m, sizeof(m), &ak);
								sendto(fd, m, sizeof(m), 0, (struct sockaddr *)&from, fl);
							}
						} else if (kind == 'P') {
							struct txp_ping pg;
							uint8_t m[TXP_PO_BYTES];

							txp_pg_get(&pg, buf, (size_t)len);
							if (memcmp(pg.cookie, ck, 8) == 0) {
								pg.b_ns = (uint64_t)now_ns();
								txp_po_put(m, sizeof(m), &pg);
								sendto(fd, m, sizeof(m), 0, (struct sockaddr *)&from, fl);
							}
						} else if (kind == 'E') {
							struct txp_en en;
							struct txp_ak ak = {0};
							uint8_t m[TXP_AK_BYTES];

							txp_en_get(&en, buf, (size_t)len);
							if (memcmp(en.cookie, ck, 8) == 0) {
								ended = true;
								ak.nonce = en.nonce;
								txp_ak_put(m, sizeof(m), &ak);
								sendto(fd, m, sizeof(m), 0, (struct sockaddr *)&from, fl);
							}
						}
					}
				}
			}
		}
		now = now_ns();
		if (subscribed && have_run && now >= next_status) {
			struct txp_st m;
			uint8_t out[TXP_ST_BYTES];

			if (!ended) {
				tbm_advance(&b, now);
			}
			tbm_status(&b, now, &m);
			m.seq = seq_n++;
			if (ended) {
				m.flags |= TXP_ST_ENDED;
				m.flags &= ~TXP_ST_RUNNING;
			}
			txp_st_put(out, sizeof(out), &m);
			for (int i = 0; i < ncs; i++) {
				if (sublen != 0 && sub.ss_family == (i == 0 ? AF_INET : AF_INET6)) {
					sendto(cs[i], out, sizeof(out), 0, (struct sockaddr *)&sub, sublen);
				}
			}
			nstat++;
			next_status += 250000;
			if (next_status < now) {
				next_status = now + 250000;
			}
		} else if (!subscribed || !have_run) {
			next_status = now + 1000000;
		}
		if (!subscribed && have_run == false && ended) {
			break;
		}
		if (!subscribed && nstat > 0 && !have_run) {
			break;
		}
	}
	if (!quiet) {
		fprintf(stderr, "tx_fake: %llu packets, %llu words, %llu bad headers, %llu wrong payloads, %llu dropped; "
			"board: underruns %u, lost %llu, late %llu, far %llu, %llu statuses\n",
			(unsigned long long)pkts, (unsigned long long)words, (unsigned long long)bad_hdr,
			(unsigned long long)wrong, (unsigned long long)dropped, b.underruns, (unsigned long long)b.lost,
			(unsigned long long)b.late, (unsigned long long)b.far, (unsigned long long)nstat);
	}
	return (bad_hdr == 0 && wrong == 0 && b.underruns == 0 && pkts > 0) ? 0 : 1;
}
