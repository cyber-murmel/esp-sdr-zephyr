/* SPDX-License-Identifier: GPL-3.0-or-later
 * Prints the board's context packet (as ctl_build_context() in src/ctl.c builds it), the command
 * packets of vrt_udp.c's send_cmd() (errors requested) and the acknowledges of ctl.c send_ack(), encoded
 * by libvrt, as hex: the golden packets of selftest.py.
 *   gcc -I$LIBVRT/include golden_gen.c $LIBVRT/src/*.c -lm -o golden_gen && ./golden_gen
 */
#include <stdio.h>
#include <vrt/vrt_init.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>

static void dump(const char *name, uint32_t *buf, int32_t w)
{
	vrt_to_big_endian(buf, w);
	printf("%s = \"", name);
	for (int i = 0; i < w * 4; i++) {
		printf("%02x", ((unsigned char *)buf)[i]);
	}
	printf("\"\n");
}

static void context(uint32_t rate, uint64_t freq, int gain, int epoch, uint32_t sec, uint64_t ps, int changed, int count)
{
	struct vrt_packet p;
	struct vrt_data_packet_payload_format *fmt = &p.if_context.data_packet_payload_format;
	uint32_t buf[64];

	vrt_init_packet(&p);
	p.header.packet_type = VRT_PT_IF_CONTEXT;
	p.header.packet_count = count & 0xf;
	p.fields.stream_id = 0x5d5d0001;
	p.header.tsi = VRT_TSI_OTHER;
	p.header.tsf = VRT_TSF_REAL_TIME;
	p.fields.integer_seconds_timestamp = sec;
	p.fields.fractional_seconds_timestamp = ps;
	p.header.nd0 = true;
	p.if_context.has.v49_spec_compliance = true;
	p.if_context.v49_spec_compliance = VRT_V49_2;
	p.if_context.context_field_change_indicator = changed;
	p.if_context.has.rf_reference_frequency = true;
	p.if_context.rf_reference_frequency = (double)freq;
	p.if_context.has.sample_rate = true;
	p.if_context.sample_rate = (double)rate;
	p.if_context.has.bandwidth = true;
	p.if_context.bandwidth = (double)rate;
	p.if_context.has.gain = true;
	p.if_context.gain.stage1 = (float)gain;
	p.if_context.has.state_and_event_indicators = true;
	p.if_context.state_and_event_indicators.user_defined = epoch;
	p.if_context.has.data_packet_payload_format = true;
	fmt->packing_method = VRT_PM_LINK_EFFICIENT;
	fmt->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
	fmt->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
	fmt->item_packing_field_size = 7;
	fmt->data_item_size = 7;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	dump("CONTEXT", buf, vrt_write_packet(&p, buf, 64, true));
}

static void command(const char *name, uint32_t id, int has_f, double f, int has_g, double g, int has_r, double r)
{
	struct vrt_packet p;
	struct vrt_command c;
	uint32_t buf[64];

	vrt_init_packet(&p);
	vrt_init_command(&c);
	p.command = &c;
	p.header.packet_type = VRT_PT_COMMAND;
	p.fields.stream_id = 0x5d5d0001;
	c.message_id = id;
	c.cam.action_mode = VRT_AM_EXECUTE;
	c.cam.execution = true;
	c.cam.errors = true;		/* as vrt_udp.c send_cmd(): ask for the error words */
	c.cam.query_state = true;	/* and the state after it, with the settings epoch */
	p.if_context.has.rf_reference_frequency = has_f;
	p.if_context.rf_reference_frequency = f;
	p.if_context.has.gain = has_g;
	p.if_context.gain.stage1 = (float)g;
	p.if_context.has.sample_rate = has_r;
	p.if_context.sample_rate = r;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	dump(name, buf, vrt_write_packet(&p, buf, 64, true));
}

/* send_ack() of src/ctl.c for a command that asked for errors: executed or refused (rate and frequency) */
static void ack(const char *name, uint32_t id, int refused)
{
	struct vrt_packet p;
	struct vrt_command c;
	uint32_t buf[64];

	vrt_init_packet(&p);
	vrt_init_command(&c);
	p.command = &c;
	p.header.packet_type = VRT_PT_COMMAND;
	p.header.acknowledge = true;
	p.fields.stream_id = 0x5d5d0001;
	c.message_id = id;
	c.cam.execution = true;
	c.cam.scheduled_or_executed = !refused;
	c.cam.errors = refused;
	if (refused) {
		c.errors.has.rf_reference_frequency = true;
		c.errors.rf_reference_frequency = VRT_WEF_PARAMETER_OUT_OF_RANGE;
		c.errors.has.sample_rate = true;
		c.errors.sample_rate = VRT_WEF_PARAMETER_OUT_OF_RANGE;
	}
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	dump(name, buf, vrt_write_packet(&p, buf, 64, true));
}

int main(void)
{
	context(8000000, 2440000000ULL, -1, 7, 123, 456000000ULL, 1, 5);
	command("COMMAND_ALL", 1, 1, 2440000000.0, 1, 30, 1, 8000000.0);
	command("COMMAND_AUTO", 2, 0, 0, 1, -1, 1, 40000000.0);
	command("COMMAND_FREQ", 3, 1, 2412000000.0, 0, 0, 0, 0);
	ack("ACK_EXECUTED", 1, 0);
	ack("ACK_REFUSED", 2, 1);
	return 0;
}
