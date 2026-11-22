// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vrt_sim: a stand-in for the apps/stream_eth board on the loopback, for testing the SoapySDR
 * module's VRT link (127.0.0.1 or ::1 only). It speaks the board's protocol as ctl.c does:
 *
 *  - the board's subscription protocol (apps/stream_eth README): "VRTS" is answered "VRTC" + 8 byte
 *    cookie (of the source address, port and a 30 s epoch under a key made at start); "VRTS" +
 *    cookie subscribes ("VRTK") or renews (no answer), "VRTB" if another host has the stream;
 *    "VRTT" + cookie takes it over ("VRTK"); "VRTX" + cookie stops (subscriber only). A stale
 *    cookie is answered with a new "VRTC". -b: another host holds the stream from the start.
 *  - a command packet to stream ID 0x5d5d0001 sets
 *    frequency (2200 to 2900 MHz), gain (0 to 71 or -1) and sample rate (the diag16 list) and is
 *    acknowledged (execution; an unsupported value leaves out "executed", as the board does, and
 *    changes nothing), a command with no settings and the query bit is answered with the state
 *    and does not subscribe. A setting that changes something starts a new run: a context packet
 *    first, then data from a new sample index (seconds since the simulator started, as the board).
 *  - no renewal for 5 s ends the stream.
 *  - data packets: header with stream ID, seconds, picoseconds, count mod 16, trailer, and int8
 *    pairs I = n & 0xff, Q = (n >> 8) & 0xff for the sample index n of the pair, so a receiver
 *    sees any gap or repeat in the samples themselves. Context packets once a second.
 *
 * Faults (all counted in the packet count and time as the board's network would): -d N drops
 * every Nth packet; -g S:K skips K packets S seconds into a run (a forced gap); -z S:L is silent
 * (no packets, no acknowledges) for L seconds starting S seconds after the start, and comes back as
 * after a reboot (defaults, no subscriber until the next VRTS);
 * -x F sends F times faster than the sample rate (a throughput test: the sample index still follows
 * the packets);
 * -r R starts at R S/s (not 20 M); -U n lets the first n executed acknowledges through before -A loses any; -S F accepts frequency F but streams nothing until the next command; -6 listens on ::1; -C n loses the first n settings commands, -A n the acknowledges of the first n executed
 * ones, -N n those of the first n refused ones; -j sends malformed datagrams in between; -o S: another host takes the stream S seconds in (VRTB for ours); -m F refuses frequencies above F Hz
 * (a board-side refusal naming the field); -w D adds D Hz to the frequency its contexts show; -t T exits at once T seconds after the start (a board dying); -P pairs per packet (720).
 *
 * Build (from the workspace root, libvrt's sources are the codec under test's counterpart):
 *   gcc -O2 -Wall -o vrt_sim vrt_sim.c -Imodules/lib/libvrt/include -Imodules/lib/libvrt/src \
 *       modules/lib/libvrt/src/vrt_*.c modules/lib/libvrt/src/timestamp_to_calendar.c -lm
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
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
#define SILENT_S  5.0

static const uint32_t RATES[] = {40000000, 20000000, 16000000, 10000000, 8000000, 5000000, 4000000,
				 3200000,  2500000,  2000000,  1600000,  1250000,  1000000, 800000,
				 640000};

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static int s;
static socklen_t ss_len(const struct sockaddr_storage *a);
static struct sockaddr_storage peer;
static socklen_t peer_len;
static bool use6;
static bool subscribed, running, held_by_other;
static double stolen_at = -1, max_freq = 2900e6, ctx_freq_off;
static bool err_f, err_g, err_r;
static int lose_cmds, lose_acks, lose_refusals;
static bool junk;
static uint64_t last_ctx_freq, last_ctx_rate;
static int last_ctx_gain;
static bool ctx_sent;
static uint8_t epoch;
static double restart_at = -1, start_delay_s = 0.030;
static bool quantize;
static int ctx_epoch_off;
static uint64_t silent_freq;
static int pass_acks;
static uint64_t key = 0x9e3779b97f4a7c15ULL;
static double last_renew, t_start, run_start, silent_from = -1, silent_len;
static uint64_t freq_hz = 2412000000ULL, idx0, sent_in_run;
static uint32_t rate = 20000000;
static int gain = -1;
static double speed = 1.0;
static int pairs = 720, drop_every, gap_at = -1, gap_n;
static unsigned long total_packets, dropped, skipped;
static uint8_t count;
static bool gap_started;
static int gap_left;

static bool silent(void)
{
	double t = now_s() - t_start;

	return silent_from >= 0 && t >= silent_from && t < silent_from + silent_len;
}

static void send_peer(const void *p, size_t n)
{
	(void)sendto(s, p, n, 0, (struct sockaddr *)&peer, peer_len);
}

static void send_context(void)
{
	struct vrt_packet p;
	uint32_t w[32];
	struct vrt_data_packet_payload_format *f = &p.if_context.data_packet_payload_format;
	int32_t n32;
	double t = now_s() - t_start;

	vrt_init_packet(&p);
	p.header.packet_type = VRT_PT_IF_CONTEXT;
	p.fields.stream_id = STREAM_ID;
	p.header.tsi = VRT_TSI_OTHER;
	p.header.tsf = VRT_TSF_REAL_TIME;
	p.fields.integer_seconds_timestamp = (uint32_t)t;
	p.fields.fractional_seconds_timestamp = (uint64_t)((t - floor(t)) * 1e12);
	p.header.nd0 = true;
	p.if_context.has.v49_spec_compliance = true;
	p.if_context.v49_spec_compliance = VRT_V49_2;
	/* as the board: the indicator is set on the context that follows a change of what it shows */
	p.if_context.context_field_change_indicator =
		ctx_sent && (freq_hz != last_ctx_freq || rate != last_ctx_rate || gain != last_ctx_gain);
	last_ctx_freq = freq_hz;
	last_ctx_rate = rate;
	last_ctx_gain = gain;
	ctx_sent = true;
	p.if_context.has.state_and_event_indicators = true;
	p.if_context.state_and_event_indicators.user_defined = (uint8_t)(epoch + ctx_epoch_off);
	p.if_context.has.rf_reference_frequency = true;
	p.if_context.rf_reference_frequency = (double)freq_hz + ctx_freq_off;
	p.if_context.has.sample_rate = true;
	p.if_context.sample_rate = rate;
	p.if_context.has.bandwidth = true;
	p.if_context.bandwidth = rate;
	p.if_context.has.gain = true;
	p.if_context.gain.stage1 = (float)gain;
	p.if_context.has.data_packet_payload_format = true;
	f->packing_method = VRT_PM_LINK_EFFICIENT;
	f->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
	f->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
	f->item_packing_field_size = 7;
	f->data_item_size = 7;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	n32 = vrt_write_packet(&p, w, 32, true);
	vrt_to_big_endian(w, n32);
	send_peer(w, (size_t)n32 * 4U);
}

static void start_run(void)
{
	/* the board's index: seconds since boot at the new rate */
	double t = now_s() - t_start;

	idx0 = (uint64_t)(t * rate);
	run_start = now_s();
	fprintf(stderr, "vrt_sim: run: freq %llu rate %u gain %d\n", (unsigned long long)freq_hz, rate, gain);
	sent_in_run = 0;
	gap_started = false;
	gap_left = 0;
	running = true;
	send_context();
}

/* The ring restarts after a start delay: a context, then data (a stopped ring sends nothing). */
static void schedule_restart(void)
{
	running = false;
	restart_at = now_s() + start_delay_s;
}

static void send_ack(const struct vrt_command *cmd, uint32_t sid, bool query, bool executed,
		     const struct sockaddr_storage *to)
{
	struct vrt_packet ack;
	struct vrt_command ac;
	uint32_t w[48];
	int32_t n32;

	vrt_init_packet(&ack);
	vrt_init_command(&ac);
	ack.command = &ac;
	ack.header.packet_type = VRT_PT_COMMAND;
	ack.header.acknowledge = true;
	ack.fields.stream_id = sid;
	ac.message_id = cmd->message_id;
	if (query) {
		struct vrt_data_packet_payload_format *f = &ack.if_context.data_packet_payload_format;

		ac.cam.query_state = true;
		ack.if_context.has.state_and_event_indicators = true;
		ack.if_context.state_and_event_indicators.user_defined = epoch;
		ack.if_context.has.rf_reference_frequency = true;
		ack.if_context.rf_reference_frequency = (double)freq_hz;
		ack.if_context.has.sample_rate = true;
		ack.if_context.sample_rate = rate;
		ack.if_context.has.gain = true;
		ack.if_context.gain.stage1 = (float)gain;
		ack.if_context.has.data_packet_payload_format = true;
		f->packing_method = VRT_PM_LINK_EFFICIENT;
		f->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
		f->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
		f->item_packing_field_size = 7;
		f->data_item_size = 7;
	} else {
		ac.cam.execution = true;
		ac.cam.scheduled_or_executed = executed;
		if (!executed && cmd->cam.errors) {
			/* as ctl.c: the error fields only for a request that asked (cam.errors) */
			ac.cam.errors = true;
			ac.errors.has.rf_reference_frequency = err_f;
			ac.errors.rf_reference_frequency = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			ac.errors.has.gain = err_g;
			ac.errors.gain = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			ac.errors.has.sample_rate = err_r;
			ac.errors.sample_rate = VRT_WEF_PARAMETER_OUT_OF_RANGE;
		}
	}
	if (!query && lose_acks > 0 && executed) {
		if (pass_acks > 0) {
			pass_acks--; /* -U n: the first n executed acknowledges go through */
		} else {
			lose_acks--;
			fprintf(stderr, "vrt_sim: ack lost\n");
			return;
		}
	}
	if (!query && lose_refusals > 0 && !executed) {
		lose_refusals--;
		fprintf(stderr, "vrt_sim: refusal lost\n");
		return;
	}
	ack.header.packet_size = (uint16_t)vrt_words_packet(&ack);
	n32 = vrt_write_packet(&ack, w, 48, true);
	if (n32 < 0) {
		return;
	}
	vrt_to_big_endian(w, n32);
	(void)sendto(s, w, (size_t)n32 * 4U, 0, (const struct sockaddr *)to, ss_len(to));
}

static bool rate_ok(uint32_t r)
{
	for (size_t k = 0; k < sizeof(RATES) / sizeof(RATES[0]); k++) {
		if (RATES[k] == r) {
			return true;
		}
	}
	return false;
}

/* address and port bytes of a peer (IPv4 or IPv6), for comparing and hashing */
static size_t addr_bytes(const struct sockaddr_storage *a, uint8_t *out)
{
	if (a->ss_family == AF_INET6) {
		const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;

		memcpy(out, &x->sin6_addr, 16);
		memcpy(out + 16, &x->sin6_port, 2);
		return 18;
	} else {
		const struct sockaddr_in *x = (const struct sockaddr_in *)a;

		memcpy(out, &x->sin_addr, 4);
		memcpy(out + 4, &x->sin_port, 2);
		return 6;
	}
}

static socklen_t ss_len(const struct sockaddr_storage *a)
{
	return a->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
}

static unsigned port_of(const struct sockaddr_storage *a)
{
	return ntohs(a->ss_family == AF_INET6 ? ((const struct sockaddr_in6 *)a)->sin6_port
					      : ((const struct sockaddr_in *)a)->sin_port);
}

static bool is_sub(const struct sockaddr_storage *from)
{
	uint8_t x[18], y[18];
	size_t n = addr_bytes(from, x);

	return subscribed && from->ss_family == peer.ss_family && n == addr_bytes(&peer, y) &&
	       memcmp(x, y, n) == 0;
}

static uint64_t cookie_for(const struct sockaddr_storage *from, uint64_t epoch)
{
	uint64_t h = key ^ epoch * 0x100000001b3ULL;

	uint8_t x[18];
	size_t n = addr_bytes(from, x);

	for (size_t k = 0; k < n; k++) {
		h = (h ^ x[k]) * 0x100000001b3ULL;
	}
	return h ^ (h >> 29);
}

static uint64_t epoch_now(void)
{
	return (uint64_t)((now_s() - t_start) / 30.0);
}

static bool cookie_ok(const struct sockaddr_storage *from, const uint8_t *c)
{
	uint64_t ck, e = epoch_now();

	memcpy(&ck, c, 8);
	return ck == cookie_for(from, e) || (e > 0 && ck == cookie_for(from, e - 1));
}

static void reply4(const struct sockaddr_storage *to, const char *tag, const uint64_t *ck)
{
	uint8_t b[12];

	memcpy(b, tag, 4);
	if (ck != NULL) {
		memcpy(b + 4, ck, 8);
	}
	(void)sendto(s, b, ck != NULL ? 12 : 4, 0, (const struct sockaddr *)to, ss_len(to));
}

/* "VRTS" or "VRTT" with a cookie (takeover) from a host that has one. */
static void subscribe(const struct sockaddr_storage *from, bool takeover)
{
	if (is_sub(from)) {
		last_renew = now_s();
		if (takeover) {
			return;
		}
		return;
	}
	if ((subscribed || held_by_other) && !takeover) {
		reply4(from, "VRTB", NULL);
		return;
	}
	held_by_other = false;
	peer = *from;
	peer_len = ss_len(from);
	subscribed = true;
	last_renew = now_s();
	fprintf(stderr, "vrt_sim: %s on port %u\n", takeover ? "takeover" : "subscriber", port_of(&peer));
	reply4(from, "VRTK", NULL);
	ctx_sent = false; /* the first context after a subscription is not a change */
	schedule_restart();
}

static void handle_cmd(uint8_t *buf, size_t len, const struct sockaddr_storage *from)
{
	uint32_t w[64];
	struct vrt_packet p;
	struct vrt_command c;
	bool err = false, changed = false, settings;

	err_f = err_g = err_r = false;
	uint64_t nf = freq_hz;
	uint32_t nr = rate;
	int ng = gain;

	if (len % 4 != 0 || len > sizeof(w)) {
		return;
	}
	memcpy(w, buf, len);
	vrt_from_big_endian(w, (int32_t)(len / 4));
	vrt_init_packet(&p);
	vrt_init_command(&c);
	p.command = &c;
	if (vrt_read_packet(w, (int32_t)(len / 4), &p, true) < 0 || !vrt_is_command(&p.header) ||
	    p.header.acknowledge) {
		fprintf(stderr, "vrt_sim: bad command\n");
		return;
	}
	if (p.header.cancellation) {
		subscribed = running = false, restart_at = -1;
		return;
	}
	settings = p.if_context.has.rf_reference_frequency || p.if_context.has.gain ||
		   p.if_context.has.sample_rate;
	if (p.if_context.has.rf_reference_frequency) {
		double f = p.if_context.rf_reference_frequency;

		if (f != floor(f) || f < 2200e6 || f > 2900e6 || f > max_freq) {
			err = true;
			err_f = true;
		} else {
			nf = (uint64_t)f;
		}
	}
	if (p.if_context.has.gain) {
		float g = p.if_context.gain.stage1;

		if (g != floorf(g) || (g != -1.0f && (g < 0 || g > 71))) {
			err = true;
			err_g = true;
		} else {
			ng = (int)g;
		}
	}
	if (p.if_context.has.sample_rate) {
		double r = p.if_context.sample_rate;

		if (r != floor(r) || !rate_ok((uint32_t)r)) {
			err = true;
			err_r = true;
		} else {
			nr = (uint32_t)r;
		}
	}
	if (silent()) {
		return;
	}
	if (!is_sub(from)) {
		/* only the subscriber's commands execute; anyone may ask for the state */
		static double qwin;
		static int qn;
		double t = now_s();

		if (t - qwin >= 1.0) {
			qwin = t;
			qn = 0;
		}
		if (c.cam.query_state && !settings && !c.cam.execution && qn++ < 10) {
			send_ack(&c, p.fields.stream_id, true, false, from);
		}
		return;
	}
	if (settings && lose_cmds > 0) {
		lose_cmds--;
		fprintf(stderr, "vrt_sim: command lost\n");
		return;
	}
	if (c.cam.action_mode == VRT_AM_EXECUTE && !err) {
		/* a change that restarts the ring moves the epoch; with -k a retune within the same kHz
		 * plan is accepted but does not restart it */
		changed = nr != rate || ng != gain ||
			  (quantize ? (nf + 500) / 1000 != (freq_hz + 500) / 1000 : nf != freq_hz);
		if (changed) {
			epoch++;
			fprintf(stderr, "vrt_sim: change accepted: epoch %u\n", epoch);
		}
		freq_hz = nf;
		rate = nr;
		gain = ng;
	} else {
		changed = false;
	}
	if (c.cam.execution) {
		send_ack(&c, p.fields.stream_id, false, c.cam.action_mode == VRT_AM_EXECUTE && !err, from);
	}
	if (c.cam.query_state) {
		send_ack(&c, p.fields.stream_id, true, false, from);
	}
	/* the ring restarts after the acknowledge: the new run and its context come after the delay */
	if (settings && changed) {
		schedule_restart();
		if (silent_freq != 0 && freq_hz == silent_freq) {
			/* -S F: accepted, but the board cannot tune it: it streams nothing and waits for the
			 * next request (esp_sdr_set_freq failing with -ERANGE) */
			running = false;
			restart_at = -1;
			fprintf(stderr, "vrt_sim: %llu accepted, no stream\n", (unsigned long long)freq_hz);
		}
	}
}

static void drain_control(void)
{
	uint8_t buf[512];
	struct sockaddr_storage from;
	socklen_t fl;
	ssize_t n;

	for (;;) {
		fl = sizeof(from);
		n = recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0) {
			return;
		}
		if (silent()) {
			continue;
		}
		if (n >= 4 && memcmp(buf, "VRT", 3) == 0 && (buf[3] == 'S' || buf[3] == 'T' || buf[3] == 'X')) {
			uint64_t ck = cookie_for(&from, epoch_now());

			if (n == 4 && buf[3] == 'S') {
				reply4(&from, "VRTC", &ck);
			} else if (n != 12 && n != 13) {
				/* malformed: nothing */
			} else if (!cookie_ok(&from, buf + 4)) {
				/* as ctlproto.c: a stale cookie in a VRTS gets a fresh one, VRTT and VRTX nothing */
				if (buf[3] == 'S') {
					reply4(&from, "VRTC", &ck);
				}
			} else if (buf[3] == 'X') {
				if (is_sub(&from)) {
					fprintf(stderr, "vrt_sim: VRTX\n");
					subscribed = running = false, restart_at = -1;
				}
			} else {
				subscribe(&from, buf[3] == 'T');
			}
		} else if (n >= 16) {
			handle_cmd(buf, (size_t)n, &from);
		}
	}
}

/* Truncated and malformed datagrams, of several kinds, in between the stream (-j). */
static void send_junk(unsigned kind)
{
	uint8_t b[256];

	memset(b, 0, sizeof(b));
	switch (kind % 8) {
	case 0: /* 3 bytes */
		send_peer("VRT", 3);
		break;
	case 1: /* data header claiming more words than the datagram has */
		put32(b, (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) | 400U);
		put32(b + 4, STREAM_ID);
		send_peer(b, 40);
		break;
	case 2: /* data of another stream */
		put32(b, (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) | 14U);
		put32(b + 4, 0x12345678U);
		send_peer(b, 56);
		break;
	case 3: /* a length that is not a whole number of words */
		put32(b, (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) | 14U);
		send_peer(b, 55);
		break;
	case 4: /* a context of 3 words */
		put32(b, (4U << 28) | 3U);
		put32(b + 4, STREAM_ID);
		put32(b + 8, 0);
		send_peer(b, 12);
		break;
	case 5: /* an acknowledge that says it is longer than it is */
		put32(b, (6U << 28) | (1U << 26) | 60U);
		put32(b + 4, STREAM_ID);
		send_peer(b, 16);
		break;
	case 6: /* "VRTC" with a cookie that is 1 byte */
		send_peer("VRTCx", 5);
		break;
	default: /* noise */
		for (int k = 0; k < 100; k++) {
			b[k] = (uint8_t)(k * 37 + 11);
		}
		b[0] = 0xff;
		send_peer(b, 100);
	}
}

int main(int argc, char **argv)
{
	int port = 4992, opt;
	double die = 0;
	struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
	static uint8_t pkts[64][2048];
	struct mmsghdr mm[64];
	struct iovec iov[64];
	double last_ctx = 0;

	while ((opt = getopt(argc, argv, "p:P:d:g:z:t:x:bo:m:w:6C:A:N:jr:D:kE:S:U:")) != -1) {
		switch (opt) {
		case 'p': port = atoi(optarg); break;
		case 'P': pairs = atoi(optarg); break;
		case 'd': drop_every = atoi(optarg); break;
		case 'g': sscanf(optarg, "%d:%d", &gap_at, &gap_n); break;
		case 'z': sscanf(optarg, "%lf:%lf", &silent_from, &silent_len); break;
		case 't': die = atof(optarg); break;
		case 'x': speed = atof(optarg); break;
		case 'b': held_by_other = true; break;
		case 'o': stolen_at = atof(optarg); break;
		case '6': use6 = true; break;
		case 'C': lose_cmds = atoi(optarg); break;
		case 'A': lose_acks = atoi(optarg); break;
		case 'N': lose_refusals = atoi(optarg); break;
		case 'j': junk = true; break;
		case 'D': start_delay_s = atof(optarg) / 1000.0; break;
		case 'k': quantize = true; break;
		case 'E': ctx_epoch_off = atoi(optarg); break;
		case 'U': pass_acks = atoi(optarg); break;
		case 'S': silent_freq = (uint64_t)atoll(optarg); break;
		case 'r': rate = (uint32_t)atol(optarg); break;
		case 'm': max_freq = atof(optarg); break;
		case 'w': ctx_freq_off = atof(optarg); break;
		default: return 2;
		}
	}
	a4.sin_port = htons((uint16_t)port);
	a6.sin6_port = htons((uint16_t)port);
	s = socket(use6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
	if (s < 0 || (use6 ? bind(s, (struct sockaddr *)&a6, sizeof(a6))
			   : bind(s, (struct sockaddr *)&a4, sizeof(a4))) != 0) {
		perror("vrt_sim: bind");
		return 2;
	}
	{
		int sz = 16 << 20;

		setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	}
	t_start = now_s();
	key ^= (uint64_t)getpid() * 0x2545F4914F6CDD1DULL ^ (uint64_t)(t_start * 1e6);
	for (;;) {
		double t = now_s();
		uint64_t due;
		int nb = 0;

		if (die > 0 && t - t_start >= die) {
			fprintf(stderr, "vrt_sim: dying: %lu packets, %lu dropped, %lu skipped\n",
				total_packets, dropped, skipped);
			_exit(0);
		}
		if (silent()) {
			/* a board that went away comes back as after a reboot: defaults, no subscriber */
			subscribed = running = false, restart_at = -1;
			key = key * 6364136223846793005ULL + 1442695040888963407ULL;
			freq_hz = 2412000000ULL;
			rate = 20000000;
			gain = -1;
			epoch = 0;
			restart_at = -1;
		}
		if (stolen_at >= 0 && t - t_start >= stolen_at) {
			/* another host took the stream (-o S): ours gets VRTB and nothing else */
			stolen_at = -1;
			held_by_other = true;
			subscribed = running = false, restart_at = -1;
		}
		drain_control();
		if (restart_at >= 0 && now_s() >= restart_at && subscribed) {
			restart_at = -1;
			start_run();
		}
		if (subscribed && t - last_renew > SILENT_S) {
			fprintf(stderr, "vrt_sim: no renewal for 5 s: stopped\n");
			subscribed = running = false, restart_at = -1;
		}
		if (!running || silent()) {
			usleep(2000);
			continue;
		}
		if (t - last_ctx >= 1.0) {
			send_context();
			last_ctx = t;
		}
		due = (uint64_t)((t - run_start) * rate * speed / (double)pairs);
		if (sent_in_run >= due) {
			usleep(50);
			continue;
		}
		while (sent_in_run < due && nb < 64) {
			uint64_t seq = sent_in_run++;
			uint64_t n = idx0 + seq * (uint64_t)pairs;
			uint64_t sec = n / rate, ps = (n % rate) * (1000000000000ULL / rate);
			uint8_t *b = pkts[nb];
			bool skip = false;

			if (gap_at >= 0 && !gap_started && t - run_start >= gap_at && gap_n > 0) {
				gap_started = true;
				gap_left = gap_n;
			}
			if (gap_left > 0) {
				gap_left--;
				skipped++;
				skip = true;
			} else if (drop_every != 0 && seq % (uint64_t)drop_every == (uint64_t)drop_every - 1U) {
				dropped++;
				skip = true;
			}
			if (!skip) {
				put32(b, (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) |
						 ((uint32_t)(count & 15U) << 16) |
						 (uint32_t)((20 + 2 * pairs + 4) / 4));
				put32(b + 4, STREAM_ID);
				put32(b + 8, (uint32_t)sec);
				put32(b + 12, (uint32_t)(ps >> 32));
				put32(b + 16, (uint32_t)ps);
				for (int k = 0; k < pairs; k++) {
					uint64_t m = n + (uint64_t)k;

					b[20 + 2 * k] = (uint8_t)m;
					b[21 + 2 * k] = (uint8_t)(m >> 8);
				}
				put32(b + 20 + 2 * pairs, (1U << 30) | (1U << 28) | (1U << 25) | (1U << 24));
				iov[nb].iov_base = b;
				iov[nb].iov_len = (size_t)(24 + 2 * pairs);
				memset(&mm[nb].msg_hdr, 0, sizeof(mm[nb].msg_hdr));
				mm[nb].msg_hdr.msg_iov = &iov[nb];
				mm[nb].msg_hdr.msg_iovlen = 1;
				mm[nb].msg_hdr.msg_name = &peer;
				mm[nb].msg_hdr.msg_namelen = peer_len;
				nb++;
				total_packets++;
			}
			count++;
		}
		{
			static unsigned jc;

			if (junk && nb > 0 && (++jc % 40U) == 0U) {
				send_junk(jc / 40U);
			}
		}
		for (int off = 0; off < nb;) {
			int r = sendmmsg(s, mm + off, (unsigned)(nb - off), 0);

			if (r < 0) {
				if (errno == EINTR || errno == EAGAIN || errno == ENOBUFS) {
					usleep(20);
					continue;
				}
				break; /* the peer is gone: its renewals stop and so do we */
			}
			off += r;
		}
	}
}
