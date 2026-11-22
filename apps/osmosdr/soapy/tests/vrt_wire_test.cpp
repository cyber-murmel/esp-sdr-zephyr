// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vrt_wire_test: ../vrt_wire.h against libvrt (the codec the board uses): the command packets the
 * module builds are decoded by libvrt and must say what was asked, and the acknowledges, query
 * states and context packets libvrt writes (as apps/stream_eth/src/ctl.c does) must be understood.
 * Prints "ok" lines, exits 1 on the first difference.
 *
 *   L=modules/lib/libvrt; for f in $L/src/vrt_*.c $L/src/timestamp_to_calendar.c; do
 *     gcc -O1 -c -I$L/include -I$L/src $f -o objs/$(basename $f).o; done
 *   g++ -std=c++17 -Wall -o vrt_wire_test vrt_wire_test.cpp objs/vrt_*.o -I$L/include -I..
 */
extern "C" {
#include <vrt/vrt_init.h>
#include <vrt/vrt_read.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>
}
#include <cstdio>
#include <cstdlib>

#include "../vrt_wire.h"

static int fails;

#define CHECK(c)                                                                                   \
	do {                                                                                       \
		if (!(c)) {                                                                        \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
			fails++;                                                                   \
		}                                                                                  \
	} while (0)

static void decode_cmd(const uint8_t *b, size_t n, vrt_packet &p, vrt_command &c)
{
	uint32_t w[64];

	memcpy(w, b, n);
	vrt_from_big_endian(w, (int32_t)(n / 4));
	vrt_init_packet(&p);
	vrt_init_command(&c);
	p.command = &c;
	CHECK(vrt_read_packet(w, (int32_t)(n / 4), &p, true) >= 0);
}

static void settings_case(bool f, bool g, bool r, double hz, int gain, double rate)
{
	vrtwire::Settings s;
	uint8_t b[vrtwire::CMD_MAX_BYTES];

	s.hasFreq = f;
	s.hasGain = g;
	s.hasRate = r;
	s.freqHz = hz;
	s.gain = gain;
	s.rateHz = rate;
	size_t n = vrtwire::buildCommand(b, sizeof(b), 77, s);
	vrt_packet p;
	vrt_command c;

	CHECK(n != 0);
	decode_cmd(b, n, p, c);
	CHECK(vrt_is_command(&p.header));
	CHECK(p.fields.stream_id == vrtwire::STREAM_ID);
	CHECK(c.message_id == 77);
	CHECK(c.cam.action_mode == VRT_AM_EXECUTE);
	CHECK(c.cam.execution == (f || g || r));
	CHECK(c.cam.errors == (f || g || r));
	CHECK(c.cam.query_state); /* with settings too: the second acknowledge carries the epoch */
	CHECK(p.if_context.has.rf_reference_frequency == f);
	CHECK(p.if_context.has.gain == g);
	CHECK(p.if_context.has.sample_rate == r);
	if (f) {
		CHECK(p.if_context.rf_reference_frequency == hz);
	}
	if (g) {
		CHECK(p.if_context.gain.stage1 == (float)gain);
		CHECK(p.if_context.gain.stage2 == 0.0f);
	}
	if (r) {
		CHECK(p.if_context.sample_rate == rate);
	}
	/* and libvrt's own encoder gives the same bytes */
	vrt_packet q;
	vrt_command qc;
	uint32_t w[64];

	vrt_init_packet(&q);
	vrt_init_command(&qc);
	q.command = &qc;
	q.header.packet_type = VRT_PT_COMMAND;
	q.fields.stream_id = vrtwire::STREAM_ID;
	qc.message_id = 77;
	qc.cam.action_mode = VRT_AM_EXECUTE;
	qc.cam.execution = f || g || r;
	qc.cam.errors = f || g || r;
	qc.cam.query_state = true;
	q.if_context.has.rf_reference_frequency = f;
	q.if_context.rf_reference_frequency = hz;
	q.if_context.has.gain = g;
	q.if_context.gain.stage1 = (float)gain;
	q.if_context.has.sample_rate = r;
	q.if_context.sample_rate = rate;
	q.header.packet_size = (uint16_t)vrt_words_packet(&q);
	int32_t wn = vrt_write_packet(&q, w, 64, true);

	vrt_to_big_endian(w, wn);
	CHECK((size_t)wn * 4U == n);
	CHECK(memcmp(w, b, n) == 0);
	printf("ok   command f=%d g=%d r=%d (%zu bytes)\n", f, g, r, n);
}

static void ack_cases(void)
{
	for (int v = 0; v < 3; v++) {
		vrt_packet p;
		vrt_command c;
		uint32_t w[48];

		vrt_init_packet(&p);
		vrt_init_command(&c);
		p.command = &c;
		p.header.packet_type = VRT_PT_COMMAND;
		p.header.acknowledge = true;
		p.fields.stream_id = vrtwire::STREAM_ID;
		c.message_id = 9;
		if (v == 2) {
			vrt_data_packet_payload_format *f = &p.if_context.data_packet_payload_format;

			c.cam.query_state = true;
			p.if_context.has.state_and_event_indicators = true;
			p.if_context.state_and_event_indicators.user_defined = 200;
			p.if_context.has.rf_reference_frequency = true;
			p.if_context.rf_reference_frequency = 2437e6;
			p.if_context.has.sample_rate = true;
			p.if_context.sample_rate = 16e6;
			p.if_context.has.gain = true;
			p.if_context.gain.stage1 = 33;
			p.if_context.has.data_packet_payload_format = true;
			f->packing_method = VRT_PM_LINK_EFFICIENT;
			f->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
			f->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
			f->item_packing_field_size = 7;
			f->data_item_size = 7;
		} else {
			c.cam.execution = true;
			c.cam.scheduled_or_executed = v == 0;
			if (v == 1) {
				c.cam.errors = true;
				c.errors.has.rf_reference_frequency = true;
				c.errors.rf_reference_frequency = VRT_WEF_PARAMETER_OUT_OF_RANGE;
				c.errors.has.sample_rate = true;
				c.errors.sample_rate = VRT_WEF_DEVICE_FAILURE;
			}
		}
		p.header.packet_size = (uint16_t)vrt_words_packet(&p);
		int32_t n = vrt_write_packet(&p, w, 48, true);

		vrt_to_big_endian(w, n);
		vrtwire::Ack a;

		CHECK(vrtwire::parseAck((const uint8_t *)w, (size_t)n * 4U, a));
		CHECK(a.messageId == 9);
		CHECK(a.execution == (v != 2));
		CHECK(a.query == (v == 2));
		CHECK(a.executed == (v == 0));
		if (v == 1) {
			CHECK(a.errors && a.errFreq && a.codeFreq == 0x10000000U);
			CHECK(a.errRate && a.codeRate == 0x40000000U);
			CHECK(!a.errGain && !a.errFormat && !a.errOther);
		}
		if (v == 2) {
			CHECK(a.state.hasEpoch && a.state.epoch == 200);
			CHECK(a.state.hasFreq && a.state.freqHz == 2437e6);
			CHECK(a.state.hasRate && a.state.rateHz == 16e6);
			CHECK(a.state.hasGain && a.state.gain == 33);
		}
		printf("ok   ack %d\n", v);
	}
}

static void context_case(double gain, bool changed)
{
	vrt_packet p;
	uint32_t w[32];
	vrt_data_packet_payload_format *f = &p.if_context.data_packet_payload_format;

	vrt_init_packet(&p);
	p.header.packet_type = VRT_PT_IF_CONTEXT;
	p.fields.stream_id = vrtwire::STREAM_ID;
	p.header.tsi = VRT_TSI_OTHER;
	p.header.tsf = VRT_TSF_REAL_TIME;
	p.fields.integer_seconds_timestamp = 1234;
	p.fields.fractional_seconds_timestamp = 567890123456ULL;
	p.header.nd0 = true;
	p.if_context.has.v49_spec_compliance = true;
	p.if_context.v49_spec_compliance = VRT_V49_2;
	p.if_context.context_field_change_indicator = changed;
	p.if_context.has.state_and_event_indicators = true;
	p.if_context.state_and_event_indicators.user_defined = 37;
	p.if_context.has.rf_reference_frequency = true;
	p.if_context.rf_reference_frequency = 2412e6;
	p.if_context.has.sample_rate = true;
	p.if_context.sample_rate = 40e6;
	p.if_context.has.bandwidth = true;
	p.if_context.bandwidth = 40e6;
	p.if_context.has.gain = true;
	p.if_context.gain.stage1 = (float)gain;
	p.if_context.has.data_packet_payload_format = true;
	f->packing_method = VRT_PM_LINK_EFFICIENT;
	f->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
	f->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
	f->item_packing_field_size = 7;
	f->data_item_size = 7;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	int32_t n = vrt_write_packet(&p, w, 32, true);

	vrt_to_big_endian(w, n);
	vrtwire::Context c;

	CHECK(vrtwire::parseContext((const uint8_t *)w, (size_t)n * 4U, c));
	CHECK(c.state.hasEpoch && c.state.epoch == 37);
	CHECK(c.hasTime && c.sec == 1234 && c.ps == 567890123456ULL);
	CHECK(c.state.hasFreq && c.state.freqHz == 2412e6);
	CHECK(c.state.hasRate && c.state.rateHz == 40e6);
	CHECK(c.state.hasBw && c.state.bwHz == 40e6);
	CHECK(c.state.hasGain && c.state.gain == (int)gain);
	printf("ok   context (gain %.0f, changed %d)\n", gain, changed);
}

static void data_case(void)
{
	uint8_t b[24 + 1440];
	uint32_t trailer = (1U << 30) | (1U << 28) | (1U << 25) | (1U << 24) | (1U << 12);

	vrtwire::put32(b, (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) | (5U << 16) | (uint32_t)(24 + 1440) / 4);
	vrtwire::put32(b + 4, vrtwire::STREAM_ID);
	vrtwire::put32(b + 8, 12);
	vrtwire::put32(b + 12, 0);
	vrtwire::put32(b + 16, 500000);
	for (int k = 0; k < 1440; k++) {
		b[20 + k] = (uint8_t)k;
	}
	vrtwire::put32(b + 20 + 1440, trailer);
	vrtwire::Data d;

	CHECK(vrtwire::parseData(b, sizeof(b), d));
	CHECK(d.count == 5 && d.sec == 12 && d.ps == 500000 && d.bodyBytes == 1440 && d.body == b + 20);
	CHECK(d.loss && !d.over);
	CHECK(vrtwire::sampleIndex(d, 40000000) == 12ULL * 40000000 + 20);
	b[2] ^= 1; /* wrong size */
	CHECK(!vrtwire::parseData(b, sizeof(b), d));
	printf("ok   data header, trailer, index\n");
}

int main()
{
	settings_case(true, true, true, 2412e6, -1, 40e6);
	settings_case(true, false, false, 2900e6, 0, 0);
	settings_case(false, true, false, 0, 71, 0);
	settings_case(false, false, true, 0, 0, 640e3);
	settings_case(false, false, false, 0, 0, 0);
	ack_cases();
	context_case(-1, false);
	context_case(40, true);
	data_case();
	printf(fails ? "FAILED: %d\n" : "all ok\n", fails);
	return fails ? 1 : 0;
}
