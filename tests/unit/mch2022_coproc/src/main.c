/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The plain logic of apps/mch2022_coproc: the I2C register map against what the stock firmware's
 * i2c_peripheral.c does, and the reset detectors against the line sequences that reach them:
 * esptool's resets and the workspace's reset scripts.
 */

#include <zephyr/ztest.h>

#include <esp_sdr/mch2022_coproc.h>

#include "regmap.h"
#include "reset_detect.h"

static struct regmap m;

/* A controller's write: the address, then the bytes. */
static void bus_write(uint8_t reg, const uint8_t *data, size_t n)
{
	regmap_write_start(&m);
	regmap_write_byte(&m, reg);
	for (size_t k = 0; k < n; k++) {
		regmap_write_byte(&m, data[k]);
	}
}

/* A controller's read: the address, then a repeated start and the bytes, as the ESP32 does. */
static void bus_read(uint8_t reg, uint8_t *data, size_t n, bool *cleared)
{
	regmap_write_start(&m);
	regmap_write_byte(&m, reg);
	for (size_t k = 0; k < n; k++) {
		bool c;

		data[k] = regmap_read_byte(&m, &c);
		if (cleared != NULL) {
			*cleared = *cleared || c;
		}
	}
}

ZTEST(regmap, test_identity)
{
	uint8_t id[4], fw;

	bus_read(REG_FW_VER, &fw, 1, NULL);
	zassert_equal(fw, 0x0C, "the stock launcher wants 0x0C at least and reads more above it");
	bus_read(COPROC_REG_ID, id, sizeof(id), NULL);
	zassert_equal(id[0], COPROC_ID);
	zassert_equal(id[1], COPROC_API_MAJOR);
	zassert_equal(id[2], COPROC_API_MINOR);
}

ZTEST(regmap, test_address_goes_on_and_wraps)
{
	const uint8_t data[] = {1, 2, 3, 4};
	uint8_t back[4];

	bus_write(254, data, sizeof(data));
	bus_read(254, back, 2, NULL);
	zassert_equal(back[0], 1);
	zassert_equal(back[1], 2);
	/* Past 255: the address wraps to 0 and 1, where register 0 stays and 1 takes the byte. */
	zassert_equal(m.regs[REG_FW_VER], REGMAP_FW_VERSION);
	zassert_equal(m.regs[REG_GPIO_DIR], 4);
}

ZTEST(regmap, test_read_only_registers_stay)
{
	static const uint8_t ro[] = {
		REG_FW_VER,      REG_GPIO_IN,    REG_INPUT1,     REG_INPUT2, REG_INTERRUPT1,
		REG_INTERRUPT2,  REG_ADC_VUSB,   REG_ADC_VBAT,   REG_USB,    REG_CRASH_DEBUG,
		REG_CHARGING,    REG_TEMP,       REG_UID,        REG_UID + 7, COPROC_REG_ID,
		COPROC_REG_FEATURES, COPROC_REG_LINK_STATE,
	};
	const uint8_t data = 0xA5;

	ARRAY_FOR_EACH(ro, k) {
		uint8_t before = m.regs[ro[k]];

		bus_write(ro[k], &data, 1);
		zassert_equal(m.regs[ro[k]], before, "register %u changed", ro[k]);
		zassert_equal(regmap_next_modified(&m, 0), -1, "register %u marked", ro[k]);
	}
}

ZTEST(regmap, test_a_read_only_register_still_moves_the_address)
{
	const uint8_t data[] = {0x11, 0x22};

	/* Register 2 is read only, 3 is not. */
	bus_write(REG_GPIO_IN, data, sizeof(data));
	zassert_equal(m.regs[REG_GPIO_IN], 0);
	zassert_equal(m.regs[REG_GPIO_OUT], 0x22);
}

ZTEST(regmap, test_writes_come_in_ascending_order)
{
	const uint8_t data = 1;

	bus_write(REG_IR_TRIGGER, &data, 1);
	bus_write(REG_RESET_LOCK, &data, 1);
	bus_write(REG_FPGA, &data, 1);
	zassert_equal(regmap_next_modified(&m, 0), REG_FPGA);
	zassert_equal(regmap_next_modified(&m, REG_FPGA + 1), REG_RESET_LOCK);
	zassert_equal(regmap_next_modified(&m, REG_RESET_LOCK + 1), REG_IR_TRIGGER);
	zassert_equal(regmap_next_modified(&m, REG_IR_TRIGGER + 1), -1);
	zassert_equal(regmap_next_modified(&m, 0), -1, "marks are taken");
}

ZTEST(regmap, test_a_written_register_reads_back)
{
	const uint8_t data = 0x5A;
	uint8_t back;

	bus_write(40, &data, 1);
	bus_read(40, &back, 1, NULL);
	zassert_equal(back, data);
}

/* As the ESP32's driver: four bytes from register 6, the last of which clears. */
ZTEST(regmap, test_reading_interrupt2_clears_both)
{
	uint8_t in[4];
	bool cleared = false;

	zassert_true(regmap_update_input(&m, REG_INPUT1, 0x01, 0));
	zassert_true(regmap_update_input(&m, REG_INPUT2, 0x10, 0));
	bus_read(REG_INPUT1, in, sizeof(in), &cleared);
	zassert_equal(in[0], 0x01);
	zassert_equal(in[1], 0x10);
	zassert_equal(in[2], 0x01, "the bits are read before they clear");
	zassert_equal(in[3], 0x10);
	zassert_true(cleared);
	zassert_equal(m.regs[REG_INTERRUPT1], 0);
	zassert_equal(m.regs[REG_INTERRUPT2], 0);
	zassert_equal(m.regs[REG_INPUT1], 0x01, "the state stays");
}

ZTEST(regmap, test_reading_interrupt1_alone_does_not_clear)
{
	uint8_t v;
	bool cleared = false;

	(void)regmap_update_input(&m, REG_INPUT1, 0x02, 0);
	bus_read(REG_INTERRUPT1, &v, 1, &cleared);
	zassert_equal(v, 0x02);
	zassert_false(cleared);
	zassert_equal(m.regs[REG_INTERRUPT1], 0x02);
}

ZTEST(regmap, test_changes_accumulate_until_read)
{
	zassert_true(regmap_update_input(&m, REG_INPUT1, 0x01, 0));
	zassert_false(regmap_update_input(&m, REG_INPUT1, 0x01, 0), "no change");
	zassert_true(regmap_update_input(&m, REG_INPUT1, 0x03, 0));
	zassert_true(regmap_update_input(&m, REG_INPUT1, 0x02, 0));
	zassert_equal(m.regs[REG_INTERRUPT1], 0x03, "both bits changed");
}

ZTEST(regmap, test_cdone_raises_no_interrupt)
{
	zassert_false(regmap_update_input(&m, REG_INPUT1, REG_INPUT1_NOT_CDONE,
					  REG_INPUT1_NOT_CDONE));
	zassert_equal(m.regs[REG_INPUT1], REG_INPUT1_NOT_CDONE);
	zassert_equal(m.regs[REG_INTERRUPT1], 0);
}

static void regmap_before(void *fixture)
{
	ARG_UNUSED(fixture);
	regmap_init(&m);
}

ZTEST_SUITE(regmap, NULL, NULL, regmap_before, NULL, NULL);

/* ---- the stock detectors ---- */

struct line_state {
	bool dtr, rts;
};

static struct reset_detect d;

/* The lines take each state; the action of the last, and none before unless @p any. */
static enum reset_action run(const struct line_state *seq, size_t n, bool any)
{
	enum reset_action last = RESET_ACTION_NONE;

	for (size_t k = 0; k < n; k++) {
		enum reset_action a = reset_detect_stock(&d, seq[k].dtr, seq[k].rts, false);

		if (!any && k + 1 < n) {
			zassert_equal(a, RESET_ACTION_NONE, "action at step %zu", k);
		}
		if (a != RESET_ACTION_NONE) {
			last = a;
		}
	}
	return last;
}

/* Linux raises both on open, and pyserial sets them as they are. */
static const struct line_state s_open[] = {{1, 1}};

/* esptool's ClassicReset, without the lines' repeated states, which the kernel does not send. */
static const struct line_state s_classic[] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

/* esptool's UnixTightReset. */
static const struct line_state s_tight[] = {{0, 0}, {1, 1}, {0, 1}, {1, 0}, {0, 0}};

ZTEST(reset_stock, test_esptool_classic_reset_reaches_download)
{
	zassert_equal(run(s_open, ARRAY_SIZE(s_open), false), RESET_ACTION_NONE);
	zassert_equal(run(s_classic, ARRAY_SIZE(s_classic), false), RESET_ACTION_NONE);
	/* The first cycle prepares it, its successor's start fires. */
	zassert_equal(reset_detect_stock(&d, 0, 1, false), RESET_ACTION_NONE);
	zassert_equal(reset_detect_stock(&d, 1, 1, false), RESET_ACTION_DOWNLOAD);
}

ZTEST(reset_stock, test_esptool_tight_reset_alone_does_not_download)
{
	zassert_equal(run(s_open, ARRAY_SIZE(s_open), true), RESET_ACTION_NONE);
	zassert_not_equal(run(s_tight, ARRAY_SIZE(s_tight), true), RESET_ACTION_DOWNLOAD,
			  "as on the stock firmware, only the classic sequence reaches it");
}

/* tmp/mch2022/cap.py: both low, RTS up, RTS down, then the close sends both low again. */
ZTEST(reset_stock, test_rts_pulse_resets_into_the_application)
{
	static const struct line_state seq[] = {{0, 0}, {0, 1}, {0, 0}, {0, 0}};

	zassert_equal(run(seq, ARRAY_SIZE(seq), false), RESET_ACTION_APP);
}

/* tmp/mch2022/repl.py keeps both up: no reset. */
ZTEST(reset_stock, test_both_lines_up_do_nothing)
{
	static const struct line_state seq[] = {{1, 1}, {1, 1}, {1, 1}};

	zassert_equal(run(seq, ARRAY_SIZE(seq), true), RESET_ACTION_NONE);
}

/* tests/integration/esdr_fpga/mch2022_pty.py opens with both low. */
ZTEST(reset_stock, test_both_lines_low_do_nothing)
{
	static const struct line_state seq[] = {{0, 0}, {0, 0}, {0, 0}};

	zassert_equal(run(seq, ARRAY_SIZE(seq), true), RESET_ACTION_NONE);
}

ZTEST(reset_stock, test_ignored_while_a_reset_runs)
{
	zassert_equal(reset_detect_stock(&d, 0, 0, false), RESET_ACTION_NONE);
	zassert_equal(reset_detect_stock(&d, 0, 1, true), RESET_ACTION_NONE);
	zassert_equal(d.app_state, 0, "no step taken");
	zassert_true(d.prev_rts, "the last state is kept");
}

ZTEST(reset_stock, test_a_reset_starts_the_detectors_over)
{
	static const struct line_state seq[] = {{0, 0}, {0, 1}, {0, 0}, {0, 0}};

	zassert_equal(run(seq, ARRAY_SIZE(seq), false), RESET_ACTION_APP);
	zassert_equal(d.app_state, 0);
	zassert_equal(d.download_state, 0);
	zassert_equal(reset_detect_stock(&d, 0, 0, false), RESET_ACTION_NONE, "not again");
}

static void reset_before(void *fixture)
{
	ARG_UNUSED(fixture);
	reset_detect_init(&d, COPROC_RESET_STOCK);
}

ZTEST_SUITE(reset_stock, NULL, NULL, reset_before, NULL, NULL);

/* ---- the circuit ---- */

ZTEST(reset_circuit, test_truth_table)
{
	struct reset_lines l;

	l = reset_detect_circuit(0, 0);
	zassert_false(l.en_low);
	zassert_false(l.io0_low);
	l = reset_detect_circuit(1, 1);
	zassert_false(l.en_low);
	zassert_false(l.io0_low);
	l = reset_detect_circuit(0, 1);
	zassert_true(l.en_low, "RTS alone: in reset");
	zassert_false(l.io0_low);
	l = reset_detect_circuit(1, 0);
	zassert_false(l.en_low);
	zassert_true(l.io0_low, "DTR alone: IO0 low");
}

ZTEST_SUITE(reset_circuit, NULL, NULL, NULL, NULL, NULL);
