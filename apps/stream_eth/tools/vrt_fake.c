// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vrt_fake: a stand-in for the board, to test vrt_udp on the loopback (127.0.0.1 only).
 *
 *   vrt_fake [-p port] [-r rate] [-n pairs per packet] [-d N] [-l N] [-t seconds]
 *            [-D seconds] [-R seconds] [-k N] [-c cookie-secret]
 *
 * Speaks the board's subscription protocol (VRTS, VRTC + cookie, VRTK, VRTB, VRTT, VRTX; commands
 * only from the subscriber, acknowledged; a silent subscriber is dropped after 5 s) and sends signal
 * data packets at the rate from the port it listens on. The samples are a byte ramp (byte k of the
 * stream is k modulo 256), so a receiver can check the file.
 *   -d N  drop every Nth packet as the network would (the counter and the time go on)
 *   -l N  the board drops every Nth packet's samples (the next packet has the sample loss flag)
 *   -D s  die after s seconds, without a word (a dead board)
 *   -R s  reboot after s seconds: the cookie secret changes and the subscription is forgotten
 *   -k N  ignore the first N command packets (a lost command)
 *   -K N  lose the first N VRTK answers
 *   -x    refuse every setting (the acknowledge says: not executed, rate out of range)
 *   -t s  end after s seconds
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <vrt/vrt_init.h>
#include <vrt/vrt_read.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>

#define STREAM_ID 0x5d5d0001U

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static double fake_freq = 2412e6, fake_gain = -1;

static uint8_t fake_epoch, run_epoch;

static void send_ctx(int s, const struct sockaddr_in *peer, uint32_t rate)
{
	struct vrt_packet p;
	uint32_t w[32];
	int32_t n32;

	vrt_init_packet(&p);
	p.header.packet_type = VRT_PT_IF_CONTEXT;
	p.fields.stream_id = STREAM_ID;
	p.header.nd0 = true;
	p.if_context.has.v49_spec_compliance = true;
	p.if_context.v49_spec_compliance = VRT_V49_2;
	p.if_context.has.sample_rate = true;
	p.if_context.sample_rate = rate;
	p.if_context.has.rf_reference_frequency = true;
	p.if_context.rf_reference_frequency = fake_freq;
	p.if_context.has.gain = true;
	p.if_context.gain.stage1 = (float)fake_gain;
	p.if_context.has.state_and_event_indicators = true;
	p.if_context.state_and_event_indicators.user_defined = run_epoch;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	n32 = vrt_write_packet(&p, w, 32, true);
	vrt_to_big_endian(w, n32);
	sendto(s, w, (size_t)n32 * 4U, 0, (const struct sockaddr *)peer, sizeof(*peer));
}

static void cookie_of(uint32_t secret, const struct sockaddr_in *a, uint8_t out[8])
{
	uint64_t h = 0x9e3779b97f4a7c15ULL ^ secret;

	h = (h ^ a->sin_addr.s_addr) * 0xff51afd7ed558ccdULL;
	h = (h ^ a->sin_port) * 0xc4ceb9fe1a85ec53ULL;
	memcpy(out, &h, 8);
}

static double nowd(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	int port = 4992, pairs = 720, drop_every = 0, loss_every = 0, opt, skip_cmds = 0, skip_k = 0, refuse = 0;
	uint32_t rate = 2000000, secret = 1;
	double secs = 8, die = 0, reboot = 0;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	struct sockaddr_in peer = {0}, from;
	socklen_t flen;
	int s;
	uint8_t buf[2048], dgram[512];
	uint64_t index = 123456789ULL, bytes = 0;
	uint8_t count = 0;
	unsigned long n = 0;
	bool loss_next = false, sub = false, rebooted = false;
	double t0 = nowd(), last_renew = 0, due0 = 0, pend_at = 0;
	uint32_t pend_rate = 0;
	bool pend_set = false;
	double pend_freq = 0, pend_gain = 0;
	bool pend_has_freq = false, pend_has_gain = false;

	while ((opt = getopt(argc, argv, "p:r:n:d:l:t:D:R:k:K:x")) != -1) {
		switch (opt) {
		case 'p': port = atoi(optarg); break;
		case 'r': rate = (uint32_t)atol(optarg); break;
		case 'n': pairs = atoi(optarg); break;
		case 'd': drop_every = atoi(optarg); break;
		case 'l': loss_every = atoi(optarg); break;
		case 't': secs = atof(optarg); break;
		case 'D': die = atof(optarg); break;
		case 'R': reboot = atof(optarg); break;
		case 'k': skip_cmds = atoi(optarg); break;
		case 'K': skip_k = atoi(optarg); break;
		case 'x': refuse = 1; break;
		default: return 2;
		}
	}
	a.sin_port = htons((uint16_t)port);
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("vrt_fake: bind");
		return 2;
	}
	for (;;) {
		double el = nowd() - t0, due;
		ssize_t len;
		size_t body = (size_t)pairs * 2U;
		bool drop, board_drop;
		uint32_t trailer;

		if (el >= secs || (die > 0 && el >= die)) {
			break;
		}
		if (reboot > 0 && el >= reboot && !rebooted) {
			rebooted = true;
			secret++;
			sub = false;
			fprintf(stderr, "vrt_fake: rebooted\n");
		}
		flen = sizeof(from);
		len = recvfrom(s, dgram, sizeof(dgram), MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
		if (len >= 4) {
			uint8_t ck[8];

			cookie_of(secret, &from, ck);
			if (len == 4 && memcmp(dgram, "VRTS", 4) == 0) {
				uint8_t r[12];

				memcpy(r, "VRTC", 4);
				memcpy(r + 4, ck, 8);
				sendto(s, r, 12, 0, (struct sockaddr *)&from, flen);
			} else if ((len == 12 || len == 13) && memcmp(dgram + 4, ck, 8) == 0 &&
				   (memcmp(dgram, "VRTS", 4) == 0 || memcmp(dgram, "VRTT", 4) == 0)) {
				bool same = sub && from.sin_port == peer.sin_port;

				if (sub && !same && memcmp(dgram, "VRTT", 4) != 0) {
					sendto(s, "VRTB", 4, 0, (struct sockaddr *)&from, flen);
				} else {
					if (!same) {
						peer = from;
						sub = true;
						n = 0;
						due0 = nowd();
						if (skip_k > 0) {
							skip_k--; /* the VRTK is lost */
						} else {
							sendto(s, "VRTK", 4, 0, (struct sockaddr *)&peer, flen);
						}
						send_ctx(s, &peer, rate);
						fprintf(stderr, "vrt_fake: subscriber on port %u\n", ntohs(peer.sin_port));
					}
					last_renew = nowd();
				}
			} else if (len == 12 && memcmp(dgram + 4, ck, 8) == 0 && memcmp(dgram, "VRTX", 4) == 0) {
				if (sub && from.sin_port == peer.sin_port) {
					sub = false;
					fprintf(stderr, "vrt_fake: stopped by the subscriber\n");
				}
			} else if (len % 4 == 0 && sub && from.sin_port == peer.sin_port && (dgram[0] >> 4) == 6) {
				uint32_t w[64];
				struct vrt_packet p;
				struct vrt_command c;

				if (skip_cmds > 0) {
					skip_cmds--;
					continue;
				}
				memcpy(w, dgram, (size_t)len);
				vrt_from_big_endian(w, (int32_t)(len / 4));
				vrt_init_packet(&p);
				vrt_init_command(&c);
				p.command = &c;
				if (vrt_read_packet(w, (int32_t)(len / 4), &p, true) >= 0) {
					struct vrt_packet ack;
					struct vrt_command ac;
					int32_t k;

					if (!refuse && (p.if_context.has.sample_rate || p.if_context.has.rf_reference_frequency ||
							p.if_context.has.gain)) {
						pend_set = true;
						if ((p.if_context.has.sample_rate && (uint32_t)p.if_context.sample_rate != rate) ||
						    (p.if_context.has.rf_reference_frequency &&
						     p.if_context.rf_reference_frequency != fake_freq) ||
						    (p.if_context.has.gain && p.if_context.gain.stage1 != fake_gain)) {
							fake_epoch++;
						}
						pend_at = nowd() + 0.05;
						pend_rate = p.if_context.has.sample_rate ? (uint32_t)p.if_context.sample_rate : rate;
						pend_has_freq = p.if_context.has.rf_reference_frequency;
						pend_freq = p.if_context.rf_reference_frequency;
						pend_has_gain = p.if_context.has.gain;
						pend_gain = p.if_context.gain.stage1;
					}
					if (p.if_context.has.sample_rate && !refuse) {
						/* the old run goes on for 50 ms, its context and its packets, and then the new
						 * one starts (the ring restarts after the acknowledge)
						 */
						pend_rate = (uint32_t)p.if_context.sample_rate;
						pend_at = nowd() + 0.05;
					}
					vrt_init_packet(&ack);
					vrt_init_command(&ac);
					ack.command = &ac;
					ack.header.packet_type = VRT_PT_COMMAND;
					ack.header.acknowledge = true;
					ack.fields.stream_id = STREAM_ID;
					ac.message_id = c.message_id;
					ac.cam.execution = true;
					ac.cam.scheduled_or_executed = !refuse;
					if (refuse && c.cam.errors) {
						ac.cam.errors = true;
						ac.errors.has.sample_rate = true;
						ac.errors.sample_rate = VRT_WEF_PARAMETER_OUT_OF_RANGE;
					}
					ack.header.packet_size = (uint16_t)vrt_words_packet(&ack);
					k = vrt_write_packet(&ack, w, 64, true);
					vrt_to_big_endian(w, k);
					sendto(s, w, (size_t)k * 4U, 0, (struct sockaddr *)&peer, sizeof(peer));
					if (c.cam.query_state) {
						/* the second acknowledge: the state, with the epoch after the command */
						struct vrt_packet qa;
						struct vrt_command qc;

						vrt_init_packet(&qa);
						vrt_init_command(&qc);
						qa.command = &qc;
						qa.header.packet_type = VRT_PT_COMMAND;
						qa.header.acknowledge = true;
						qa.fields.stream_id = STREAM_ID;
						qc.message_id = c.message_id;
						qc.cam.query_state = true;
						qa.if_context.has.state_and_event_indicators = true;
						qa.if_context.state_and_event_indicators.user_defined = fake_epoch;
						qa.header.packet_size = (uint16_t)vrt_words_packet(&qa);
						k = vrt_write_packet(&qa, w, 64, true);
						vrt_to_big_endian(w, k);
						sendto(s, w, (size_t)k * 4U, 0, (struct sockaddr *)&peer, sizeof(peer));
					}
				}
			}
		}
		if (pend_set && nowd() >= pend_at) {
			rate = pend_rate;
			run_epoch = fake_epoch;
			pend_set = false;
			pend_rate = 0;
			if (pend_has_freq) {
				fake_freq = pend_freq;
			}
			if (pend_has_gain) {
				fake_gain = pend_gain;
			}
			index += 3000000ULL;
			loss_next = true;
			n = 0;
			due0 = nowd();
			send_ctx(s, &peer, rate);
		}
		if (!sub) {
			nanosleep(&(struct timespec){0, 200000}, NULL);
			continue;
		}
		if (nowd() - last_renew > 5.0) {
			sub = false;
			fprintf(stderr, "vrt_fake: subscriber silent, dropped\n");
			continue;
		}
		due = due0 + (double)n * (double)pairs / (double)rate;
		if (nowd() < due) {
			nanosleep(&(struct timespec){0, 100000}, NULL);
			continue;
		}
		drop = drop_every != 0 && n % (unsigned long)drop_every == (unsigned long)drop_every - 1UL;
		board_drop = loss_every != 0 && n % (unsigned long)loss_every == (unsigned long)loss_every - 1UL;
		trailer = (1U << 30) | (1U << 28) | (1U << 25) | (1U << 24) | ((1U << 30) >> 12) |
			  (loss_next ? (1U << 24) >> 12 : 0U);
		if (board_drop) {
			/* the board drops this packet's samples; the next carries the flag */
			index += (uint64_t)pairs;
			bytes += body;
			loss_next = true;
			n++;
			continue;
		}
		{
			uint64_t sec = index / rate, ps = (index % rate) * (1000000000000ULL / rate);

			put32(buf, (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) |
				   ((uint32_t)(count & 15U) << 16) | (uint32_t)((20 + body + 4) / 4));
			put32(buf + 4, STREAM_ID);
			put32(buf + 8, (uint32_t)sec);
			put32(buf + 12, (uint32_t)(ps >> 32));
			put32(buf + 16, (uint32_t)ps);
		}
		for (size_t k = 0; k < body; k++) {
			buf[20 + k] = (uint8_t)(bytes + k);
		}
		put32(buf + 20 + body, trailer);
		if (!drop) {
			sendto(s, buf, 24 + body, 0, (struct sockaddr *)&peer, sizeof(peer));
		}
		loss_next = false;
		count++;
		index += (uint64_t)pairs;
		bytes += body;
		n++;
	}
	fprintf(stderr, "vrt_fake: done\n");
	return 0;
}
