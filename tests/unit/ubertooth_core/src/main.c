/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the portable core of apps/ubertooth (ubt_ctrl, ubt_clock,
 * ubt_queue, ubt_regs): the vendor requests answer as upstream's firmware
 * does, the clock fields wrap where the host expects, the queue keeps order
 * and flags overflow. Runs on native_sim.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "ubt_clock.h"
#include "ubt_ctrl.h"
#include "ubt_dev.h"
#include "ubt_queue.h"
#include "ubt_regs.h"

#define CYC_PER_100NS 24U

static struct ubt_dev dev;
static usb_pkt_rx slots[UBT_QUEUE_SLOTS];
static uint64_t cycles;
static unsigned int kicks;
static bool leds[3];
static bool reset_dfu_seen;

static uint64_t plat_cycles(void)
{
	return cycles;
}

static void plat_kick(void)
{
	kicks++;
}

static void plat_led(unsigned int led, bool on)
{
	leds[led] = on;
}

static bool plat_reset(bool dfu)
{
	reset_dfu_seen = dfu;
	return true;
}

static void plat_serial(uint8_t serial[16])
{
	for (unsigned int i = 0; i < 16U; i++) {
		serial[i] = (uint8_t)(0xa0U + i);
	}
}

static const struct ubt_platform plat = {
	.led = plat_led,
	.kick = plat_kick,
	.reset = plat_reset,
	.serial = plat_serial,
	.cycles = plat_cycles,
	.rev = "esdr-test",
	.compile_info = "ubertooth esdr-test (unit) now",
	.part_num = 0x12345678U,
};

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&dev, 0, sizeof(dev));
	memset(leds, 0, sizeof(leds));
	cycles = 1000;
	kicks = 0;
	reset_dfu_seen = false;
	dev.plat = &plat;
	dev.supported_modes = 0xffffffffU;
	ubt_clock_init(&dev.clock, CYC_PER_100NS, cycles);
	ubt_queue_init(&dev.queue, slots);
	ubt_regs_init(&dev.regs);
	ubt_dev_reset(&dev);
	dev.le.access_address = 0x8e89bed6U;
}

ZTEST_SUITE(ubertooth_core, NULL, NULL, before_each, NULL, NULL);

static int req_in(uint8_t request, uint16_t value, uint16_t index, uint8_t *buf, size_t len)
{
	struct ubt_req r = {.request = request, .in = true, .value = value, .index = index,
			    .length = (uint16_t)len};

	return ubt_ctrl(&dev, &r, buf, len);
}

static int req_out(uint8_t request, uint16_t value, uint16_t index, const uint8_t *buf, size_t len)
{
	struct ubt_req r = {.request = request, .in = false, .value = value, .index = index,
			    .length = (uint16_t)len};

	return ubt_ctrl(&dev, &r, (uint8_t *)buf, len);
}

/* ---- known requests -------------------------------------------------------------------- */

ZTEST(ubertooth_core, test_known_requests)
{
	for (unsigned int i = 0; i <= UBERTOOTH_XMAS; i++) {
		zassert_equal(ubt_ctrl_known((uint8_t)i), i != 64U, "request %u", i);
	}
	zassert_false(ubt_ctrl_known(74));
	zassert_false(ubt_ctrl_known(0xff));
}

ZTEST(ubertooth_core, test_unsupported_requests_stall)
{
	uint8_t buf[64] = {0};

	zassert_equal(req_out(UBERTOOTH_SET_ISP, 0, 0, NULL, 0), -ENOTSUP);
	zassert_equal(req_out(BOOTLOADER_FLASH, 0, 0, NULL, 0), -ENOTSUP);
	zassert_equal(req_out(UBERTOOTH_BTLE_PHY, 0, 0, NULL, 0), -ENOTSUP);
	zassert_equal(req_out(UBERTOOTH_RFCAT_SUBCMD, 1, 0, buf, 4), -ENOTSUP);
	zassert_equal(req_out(UBERTOOTH_JAM_MODE, JAM_ONCE, 0, NULL, 0), -ENOTSUP);
	zassert_equal(req_out(UBERTOOTH_JAM_MODE, JAM_CONTINUOUS, 0, NULL, 0), -ENOTSUP);
	zassert_equal(req_out(UBERTOOTH_JAM_MODE, JAM_NONE, 0, NULL, 0), 0);
}

/* ---- mode requests --------------------------------------------------------------------- */

ZTEST(ubertooth_core, test_mode_requests_and_gating)
{
	struct {
		uint8_t req;
		uint8_t mode;
		const uint8_t *data;
		size_t len;
		uint16_t value;
		uint16_t index;
	} cases[] = {
		{UBERTOOTH_RX_SYMBOLS, MODE_RX_SYMBOLS, NULL, 0, 0, 0},
		{UBERTOOTH_TX_TEST, MODE_TX_TEST, NULL, 0, 0, 0},
		{UBERTOOTH_SPECAN, MODE_SPECAN, NULL, 0, 2402, 2480},
		{UBERTOOTH_LED_SPECAN, MODE_LED_SPECAN, NULL, 0, 20, 0},
		{UBERTOOTH_BTLE_SNIFFING, MODE_BT_FOLLOW_LE, NULL, 0, 1, 0},
		{UBERTOOTH_BTLE_PROMISC, MODE_BT_PROMISC_LE, NULL, 0, 0, 0},
		{UBERTOOTH_XMAS, MODE_XMAS, NULL, 0, 0, 0},
		{UBERTOOTH_RX_GENERIC, MODE_RX_GENERIC, NULL, 0, 0, 0},
		{UBERTOOTH_AFH, MODE_AFH, NULL, 0, 0, 0},
		{UBERTOOTH_EGO, MODE_EGO, NULL, 0, 0, 0},
		{UBERTOOTH_RANGE_TEST, MODE_RANGE_TEST, NULL, 0, 0, 0},
		{UBERTOOTH_REPEATER, MODE_REPEATER, NULL, 0, 0, 0},
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		dev.supported_modes = 0;
		dev.requested_mode = MODE_IDLE;
		zassert_equal(req_out(cases[i].req, cases[i].value, cases[i].index, cases[i].data,
				      cases[i].len), -ENOTSUP, "request %u without mode", cases[i].req);
		zassert_equal(dev.requested_mode, MODE_IDLE);

		dev.supported_modes = UBT_MODE_BIT(cases[i].mode);
		kicks = 0;
		zassert_equal(req_out(cases[i].req, cases[i].value, cases[i].index, cases[i].data,
				      cases[i].len), 0, "request %u", cases[i].req);
		zassert_equal(dev.requested_mode, cases[i].mode, "request %u", cases[i].req);
		zassert_true(kicks > 0);
	}
}

ZTEST(ubertooth_core, test_stop_and_reset)
{
	dev.requested_mode = MODE_RX_SYMBOLS;
	zassert_equal(req_out(UBERTOOTH_STOP, 0, 0, NULL, 0), 0);
	zassert_equal(dev.requested_mode, MODE_IDLE);

	zassert_equal(req_out(UBERTOOTH_RESET, 0, 0, NULL, 0), 0);
	zassert_equal(dev.requested_mode, MODE_RESET);
	zassert_false(dev.reset_dfu);

	zassert_equal(req_out(UBERTOOTH_FLASH, 0, 0, NULL, 0), 0);
	zassert_equal(dev.requested_mode, MODE_RESET);
	zassert_true(dev.reset_dfu);
}

ZTEST(ubertooth_core, test_specan_arguments)
{
	zassert_equal(req_out(UBERTOOTH_SPECAN, 2048, 2480, NULL, 0), -EINVAL);
	zassert_equal(req_out(UBERTOOTH_SPECAN, 2402, 3073, NULL, 0), -EINVAL);
	zassert_equal(req_out(UBERTOOTH_SPECAN, 2480, 2402, NULL, 0), -EINVAL);
	zassert_equal(req_out(UBERTOOTH_SPECAN, 2402, 2480, NULL, 0), 0);
	zassert_equal(dev.low_freq, 2402);
	zassert_equal(dev.high_freq, 2480);
	zassert_equal(req_out(UBERTOOTH_SPECAN, 2049, 3072, NULL, 0), 0);
}

ZTEST(ubertooth_core, test_led_specan_threshold)
{
	zassert_equal(req_out(UBERTOOTH_LED_SPECAN, 257, 0, NULL, 0), -EINVAL);
	zassert_equal(req_out(UBERTOOTH_LED_SPECAN, 30, 0, NULL, 0), 0);
	zassert_equal(dev.rssi_threshold, 54 - 30);
}

/* ---- channel --------------------------------------------------------------------------- */

ZTEST(ubertooth_core, test_channel_set_get_clamp_sweep)
{
	uint8_t buf[2];

	zassert_equal(req_in(UBERTOOTH_GET_CHANNEL, 0, 0, buf, 2), 2);
	zassert_equal(buf[0] | (buf[1] << 8), 2441, "default channel");

	zassert_equal(req_out(UBERTOOTH_SET_CHANNEL, 2402, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_CHANNEL, 0, 0, buf, 2), 2);
	zassert_equal(buf[0] | (buf[1] << 8), 2402);
	zassert_equal(dev.hop_mode, HOP_NONE);
	zassert_equal(dev.le_adv_channel, 2402);

	/* Below and above the CC2400 window clamp. */
	zassert_equal(req_out(UBERTOOTH_SET_CHANNEL, 2000, 0, NULL, 0), 0);
	zassert_equal(dev.channel, UBT_MIN_FREQ);
	zassert_equal(req_out(UBERTOOTH_SET_CHANNEL, 2794, 0, NULL, 0), 0);
	zassert_equal(dev.channel, UBT_MAX_FREQ);

	/* 9999 is the band sweep, starting at 2402. */
	uint32_t gen = dev.cfg_gen;

	zassert_equal(req_out(UBERTOOTH_SET_CHANNEL, 9999, 0, NULL, 0), 0);
	zassert_equal(dev.hop_mode, HOP_SWEEP);
	zassert_equal(dev.channel, 2402);
	zassert_true(dev.cfg_gen != gen);
}

ZTEST(ubertooth_core, test_channel_deferred_while_following_le)
{
	dev.mode = MODE_BT_FOLLOW_LE;
	dev.channel = 2426;
	zassert_equal(req_out(UBERTOOTH_SET_CHANNEL, 2480, 0, NULL, 0), 0);
	zassert_equal(dev.channel, 2426, "the following mode applies the change itself");
	zassert_equal(dev.requested_channel, 2480);
	zassert_equal(dev.le_adv_channel, 2480);
}

/* ---- information requests -------------------------------------------------------------- */

ZTEST(ubertooth_core, test_ping_and_misc_replies)
{
	uint8_t buf[300];

	zassert_equal(req_in(UBERTOOTH_PING, 0, 0, buf, 0), 0);

	zassert_equal(req_in(UBERTOOTH_GET_BOARD_ID, 0, 0, buf, 1), 1);
	zassert_equal(buf[0], 1, "Ubertooth One");

	zassert_equal(req_in(UBERTOOTH_GET_SERIAL, 0, 0, buf, 17), 17);
	zassert_equal(buf[0], 0, "IAP status");
	zassert_equal(buf[1], 0xa0);
	zassert_equal(buf[16], 0xaf);
	zassert_equal(req_in(UBERTOOTH_GET_SERIAL, 0, 0, buf, 16), -EINVAL);

	zassert_equal(req_in(UBERTOOTH_GET_PARTNUM, 0, 0, buf, 5), 5);
	zassert_equal(buf[0], 0);
	zassert_equal(buf[1] | (buf[2] << 8) | (buf[3] << 16) | ((uint32_t)buf[4] << 24), 0x12345678U);

	zassert_equal(req_in(UBERTOOTH_DO_SOMETHING_REPLY, 0, 0, buf, 2), 2);
	zassert_equal(buf[0], 0x13);
	zassert_equal(buf[1], 0x37);

	zassert_equal(req_in(UBERTOOTH_RANGE_CHECK, 0, 0, buf, 5), 5);
}

ZTEST(ubertooth_core, test_rev_and_compile_info)
{
	uint8_t buf[258];
	int n;

	/* The host asks for 258 bytes and reads result[2] as the length. */
	n = req_in(UBERTOOTH_GET_REV_NUM, 0, 0, buf, sizeof(buf));
	zassert_equal(n, 2 + 1 + (int)strlen("esdr-test"));
	zassert_equal(buf[0], 0);
	zassert_equal(buf[1], 0);
	zassert_equal(buf[2], strlen("esdr-test"));
	zassert_mem_equal(&buf[3], "esdr-test", strlen("esdr-test"));

	n = req_in(UBERTOOTH_GET_COMPILE_INFO, 0, 0, buf, 256);
	zassert_equal(buf[0], strlen(plat.compile_info));
	zassert_equal(n, 1 + (int)strlen(plat.compile_info));
	zassert_mem_equal(&buf[1], plat.compile_info, strlen(plat.compile_info));

	/* A short host buffer truncates the string, never overruns. */
	n = req_in(UBERTOOTH_GET_REV_NUM, 0, 0, buf, 6);
	zassert_equal(n, 6);
	zassert_equal(buf[2], 3);
}

ZTEST(ubertooth_core, test_leds)
{
	uint8_t b;

	zassert_equal(req_out(UBERTOOTH_SET_USRLED, 1, 0, NULL, 0), 0);
	zassert_equal(req_out(UBERTOOTH_SET_TXLED, 1, 0, NULL, 0), 0);
	zassert_true(leds[0]);
	zassert_false(leds[1]);
	zassert_true(leds[2]);
	zassert_equal(req_in(UBERTOOTH_GET_USRLED, 0, 0, &b, 1), 1);
	zassert_equal(b, 1);
	zassert_equal(req_in(UBERTOOTH_GET_RXLED, 0, 0, &b, 1), 1);
	zassert_equal(b, 0);
	zassert_equal(req_out(UBERTOOTH_SET_USRLED, 0, 0, NULL, 0), 0);
	zassert_false(leds[0]);
}

ZTEST(ubertooth_core, test_pa_hgm_1v8_modulation_squelch)
{
	uint8_t b;

	zassert_equal(req_out(UBERTOOTH_SET_PAEN, 1, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_PAEN, 0, 0, &b, 1), 1);
	zassert_equal(b, 1);
	zassert_equal(req_out(UBERTOOTH_SET_HGM, 1, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_HGM, 0, 0, &b, 1), 1);
	zassert_equal(b, 1);
	zassert_equal(req_out(UBERTOOTH_SET_1V8, 1, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_1V8, 0, 0, &b, 1), 1);
	zassert_equal(b, 1);

	zassert_equal(req_out(UBERTOOTH_SET_PALEVEL, 7, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_PALEVEL, 0, 0, &b, 1), 1);
	zassert_equal(b, 7);
	zassert_equal(req_out(UBERTOOTH_SET_PALEVEL, 8, 0, NULL, 0), -EINVAL);

	zassert_equal(req_out(UBERTOOTH_SET_MOD, MOD_BT_LOW_ENERGY, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_MOD, 0, 0, &b, 1), 1);
	zassert_equal(b, MOD_BT_LOW_ENERGY);

	/* The squelch is a signed dBm value in wValue. */
	zassert_equal(req_in(UBERTOOTH_GET_SQUELCH, 0, 0, &b, 1), 1);
	zassert_equal((int8_t)b, -120, "no squelch by default");
	zassert_equal(req_out(UBERTOOTH_SET_SQUELCH, (uint16_t)(int16_t)-70, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_SQUELCH, 0, 0, &b, 1), 1);
	zassert_equal((int8_t)b, -70);
}

/* ---- LE state -------------------------------------------------------------------------- */

ZTEST(ubertooth_core, test_access_address_and_crc_verify)
{
	uint8_t buf[4] = {0xd6, 0xbe, 0x89, 0x8e};
	uint8_t out[4];

	zassert_equal(req_in(UBERTOOTH_GET_ACCESS_ADDRESS, 0, 0, out, 4), 4);
	zassert_mem_equal(out, buf, 4);

	buf[0] = 0x78;
	buf[1] = 0x56;
	buf[2] = 0x34;
	buf[3] = 0x12;
	zassert_equal(req_out(UBERTOOTH_SET_ACCESS_ADDRESS, 0, 0, buf, 4), 0);
	zassert_equal(dev.le.access_address, 0x12345678U);
	zassert_true(dev.le.target.set);
	zassert_equal(req_out(UBERTOOTH_SET_ACCESS_ADDRESS, 0, 0, buf, 3), -EINVAL);

	zassert_equal(req_in(UBERTOOTH_GET_CRC_VERIFY, 0, 0, out, 1), 1);
	zassert_equal(out[0], 0);
	zassert_equal(req_out(UBERTOOTH_SET_CRC_VERIFY, 1, 0, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_GET_CRC_VERIFY, 0, 0, out, 1), 1);
	zassert_equal(out[0], 1);
}

ZTEST(ubertooth_core, test_btle_set_target_masks)
{
	/* data: MAC as the tool sends it, then the mask length in bits. */
	uint8_t d[7] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 48};

	zassert_equal(req_out(UBERTOOTH_BTLE_SET_TARGET, 0, 0, d, 7), 0);
	zassert_true(dev.le.target.set);
	/* Stored reversed, so the first octet sits at the highest index. */
	zassert_equal(dev.le.target.addr[5], 0xaa);
	zassert_equal(dev.le.target.addr[0], 0xff);
	for (unsigned int i = 0; i < 6U; i++) {
		zassert_equal(dev.le.target.mask[i], 0xff);
	}

	d[6] = 20;
	zassert_equal(req_out(UBERTOOTH_BTLE_SET_TARGET, 0, 0, d, 7), 0);
	zassert_equal(dev.le.target.mask[5], 0xff);
	zassert_equal(dev.le.target.mask[4], 0xff);
	zassert_equal(dev.le.target.mask[3], 0xf0);
	zassert_equal(dev.le.target.mask[2], 0x00);
	zassert_equal(dev.le.target.addr[3], 0xc0, "masked 0xcc & 0xf0");
	zassert_equal(dev.le.target.addr[2], 0x00);

	/* A mask of less than eight bits also clears the stray bits of the first octet (upstream left them). */
	d[6] = 3;
	zassert_equal(req_out(UBERTOOTH_BTLE_SET_TARGET, 0, 0, d, 7), 0);
	zassert_equal(dev.le.target.mask[5], 0xe0);
	zassert_equal(dev.le.target.addr[5], 0xa0);

	d[6] = 49;
	zassert_equal(req_out(UBERTOOTH_BTLE_SET_TARGET, 0, 0, d, 7), -EINVAL);

	d[6] = 0;
	zassert_equal(req_out(UBERTOOTH_BTLE_SET_TARGET, 0, 0, d, 7), 0);
	zassert_false(dev.le.target.set);
}

ZTEST(ubertooth_core, test_slave_and_adv_data)
{
	uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
	uint8_t adv[255];

	memset(adv, 0x5a, sizeof(adv));
	zassert_equal(req_out(UBERTOOTH_BTLE_SLAVE, 0, 0, mac, 6), 0);
	zassert_mem_equal(dev.slave_mac, mac, 6);
	zassert_equal(dev.requested_mode, MODE_BT_SLAVE_LE);
	zassert_equal(req_out(UBERTOOTH_BTLE_SLAVE, 0, 0, mac, 5), -EINVAL);

	zassert_equal(req_out(UBERTOOTH_LE_SET_ADV_DATA, 0, 0, adv, 255), 0);
	zassert_equal(dev.adv_len, 255);
	zassert_equal(dev.adv_data[254], 0x5a);
	zassert_equal(req_out(UBERTOOTH_LE_SET_ADV_DATA, 0, 0, adv, 256), -EINVAL);
}

ZTEST(ubertooth_core, test_cancel_follow_and_hop)
{
	zassert_equal(req_out(UBERTOOTH_CANCEL_FOLLOW, 0, 0, NULL, 0), 0);
	zassert_true(dev.cancel_follow);
	zassert_equal(req_out(UBERTOOTH_HOP, 0, 0, NULL, 0), 0);
	zassert_equal(dev.do_hop, 1);
}

/* ---- BR and AFH state ------------------------------------------------------------------ */

ZTEST(ubertooth_core, test_bdaddr_and_afh_map)
{
	uint8_t d[16];

	for (unsigned int i = 0; i < 16U; i++) {
		d[i] = (uint8_t)(i + 1U);
	}
	zassert_equal(req_out(UBERTOOTH_SET_BDADDR, 0, 0, d, 16), 0);
	zassert_equal(dev.target.address, 0x0807060504030201ULL);
	zassert_equal(dev.target.syncword, 0x100f0e0d0c0b0a09ULL);
	zassert_equal(req_out(UBERTOOTH_SET_BDADDR, 0, 0, d, 15), -EINVAL);

	uint8_t map[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

	zassert_equal(req_out(UBERTOOTH_SET_AFHMAP, 0, 0, map, 10), 0);
	zassert_mem_equal(dev.afh_map, map, 10);
	zassert_true(dev.afh_enabled);
	zassert_equal(req_out(UBERTOOTH_CLEAR_AFHMAP, 0, 0, NULL, 0), 0);
	zassert_equal(dev.afh_map[9], 0);
	zassert_false(dev.afh_enabled);

	zassert_equal(req_out(UBERTOOTH_AFH, 0, 0, NULL, 0), 0);
	zassert_equal(dev.hop_mode, HOP_AFH);
	zassert_true(dev.afh_enabled);
}

/* ---- registers ------------------------------------------------------------------------- */

ZTEST(ubertooth_core, test_registers)
{
	uint8_t buf[200];
	uint8_t wr[9] = {0x20, 0x04, 0x61, 0x2c, 0xbe, 0xef, 0x2d, 0x12, 0x34};

	zassert_equal(req_out(UBERTOOTH_WRITE_REGISTER, UBT_REG_FSDIV, 0x1234, NULL, 0), 0);
	zassert_equal(req_in(UBERTOOTH_READ_REGISTER, UBT_REG_FSDIV, 0, buf, 2), 2);
	zassert_equal(buf[0], 0x12);
	zassert_equal(buf[1], 0x34);

	zassert_equal(req_out(UBERTOOTH_WRITE_REGISTERS, 3, 0, wr, 9), 0);
	zassert_equal(ubt_regs_read(&dev.regs, 0x20), 0x0461);
	zassert_equal(ubt_regs_read(&dev.regs, UBT_REG_SYNCL), 0xbeef);
	zassert_equal(ubt_regs_read(&dev.regs, UBT_REG_SYNCH), 0x1234);
	zassert_equal(req_out(UBERTOOTH_WRITE_REGISTERS, 3, 0, wr, 8), -EINVAL);

	/* 45 records of {reg, hi, lo}, as upstream reports. */
	int n = req_in(UBERTOOTH_READ_ALL_REGISTERS, 0, 0, buf, sizeof(buf));

	zassert_equal(n, 0x2d * 3);
	zassert_equal(buf[0], 0);
	zassert_equal(buf[3 * 0x20], 0x20);
	zassert_equal(buf[3 * 0x20 + 1], 0x04);
	zassert_equal(buf[3 * 0x20 + 2], 0x61);

	/* Registers past the file read as 0 and ignore writes. */
	zassert_equal(req_in(UBERTOOTH_READ_REGISTER, 0x70, 0, buf, 2), 2);
	zassert_equal(buf[0] | buf[1], 0);
}

/* ---- generic TX ------------------------------------------------------------------------ */

ZTEST(ubertooth_core, test_tx_generic_packet)
{
	generic_tx_packet p = {.synch = 0x1234, .syncl = 0x5678, .channel = 2420, .length = 3,
			       .pa_level = 4, .data = {9, 8, 7}};
	generic_tx_packet got;

	zassert_equal(req_out(UBERTOOTH_TX_GENERIC_PACKET, 0, 0, (uint8_t *)&p, 8 + 3), 0);
	memcpy(&got, &dev.tx_pkt, sizeof(got));
	zassert_equal(got.channel, 2420);
	zassert_equal(got.length, 3);
	zassert_equal(got.pa_level, 4, "the field upstream's length arithmetic misses");
	zassert_equal(got.data[2], 7, "the last data byte upstream's length arithmetic loses");
	zassert_equal(req_out(UBERTOOTH_TX_GENERIC_PACKET, 0, 0, (uint8_t *)&p, 4), -EINVAL);
}

/* ---- clock requests -------------------------------------------------------------------- */

ZTEST(ubertooth_core, test_clock_requests)
{
	uint8_t buf[4];
	uint8_t set[4] = {0x78, 0x56, 0x34, 0x12};

	zassert_equal(req_out(UBERTOOTH_SET_CLOCK, 0, 0, set, 4), 0);
	zassert_equal(req_in(UBERTOOTH_GET_CLOCK, 0, 0, buf, 4), 4);
	zassert_equal(buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24), 0x12345678U);

	/* One CLKN tick (312.5 us = 3125 * 100 ns = 75000 cycles) later. */
	cycles += 75000;
	zassert_equal(req_in(UBERTOOTH_GET_CLOCK, 0, 0, buf, 4), 4);
	zassert_equal(buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24), 0x12345679U);

	/* TRIM_CLOCK takes the big endian offset; FIX_CLOCK_DRIFT the signed ppm. */
	uint8_t trim[2] = {0x0b, 0xb8}; /* 3000 */

	zassert_equal(req_out(UBERTOOTH_TRIM_CLOCK, 0, 0, trim, 2), 0);
	uint8_t drift[2] = {0xff, 0x9c}; /* -100 */

	zassert_equal(req_out(UBERTOOTH_FIX_CLOCK_DRIFT, 0, 0, drift, 2), 0);
	zassert_equal(dev.clock.drift_ppm, -100);
	zassert_equal(req_out(UBERTOOTH_TRIM_CLOCK, 0, 0, trim, 1), -EINVAL);
}

ZTEST(ubertooth_core, test_start_hopping)
{
	/* clkn_offset 0x00000010 (big endian), clk100ns_offset 0 */
	uint8_t d[6] = {0, 0, 0, 0x10, 0, 0};
	uint32_t before = ubt_clock_clkn(&dev.clock, cycles);

	dev.supported_modes = 0;
	zassert_equal(req_out(UBERTOOTH_START_HOPPING, 0, 0, d, 6), -ENOTSUP);
	zassert_equal(ubt_clock_clkn(&dev.clock, cycles), before, "nothing happens without the mode");

	dev.supported_modes = UBT_MODE_BIT(MODE_BT_FOLLOW);
	zassert_equal(req_out(UBERTOOTH_START_HOPPING, 0, 0, d, 6), 0);
	zassert_equal(ubt_clock_clkn(&dev.clock, cycles), before + 0x10);
	zassert_equal(dev.hop_mode, HOP_BLUETOOTH);
	zassert_equal(dev.requested_mode, MODE_BT_FOLLOW);
	zassert_equal(req_out(UBERTOOTH_START_HOPPING, 0, 0, d, 5), -EINVAL);
}

/* ---- POLL and the queue ---------------------------------------------------------------- */

static void put(uint8_t tag)
{
	usb_pkt_rx *p = ubt_queue_alloc(&dev.queue);

	zassert_not_null(p);
	memset(p, 0, sizeof(*p));
	p->pkt_type = LE_PACKET;
	p->data[0] = tag;
	ubt_queue_commit(&dev.queue);
}

ZTEST(ubertooth_core, test_poll)
{
	uint8_t buf[64];

	/* Empty: one zero byte, which the host reads as "no packet". */
	zassert_equal(req_in(UBERTOOTH_POLL, 0, 0, buf, 64), 1);
	zassert_equal(buf[0], 0);

	put(0x11);
	put(0x22);
	zassert_equal(req_in(UBERTOOTH_POLL, 0, 0, buf, 64), 64);
	zassert_equal(buf[0], LE_PACKET);
	zassert_equal(buf[14], 0x11, "data[0] sits at offset 14");
	zassert_equal(req_in(UBERTOOTH_POLL, 0, 0, buf, 64), 64);
	zassert_equal(buf[14], 0x22);
	zassert_equal(req_in(UBERTOOTH_POLL, 0, 0, buf, 64), 1);
}

ZTEST(ubertooth_core, test_queue_full_and_overflow_flag)
{
	for (unsigned int i = 0; i < UBT_QUEUE_SLOTS - 1U; i++) {
		put((uint8_t)i);
	}
	zassert_equal(ubt_queue_count(&dev.queue), UBT_QUEUE_SLOTS - 1U);
	zassert_is_null(ubt_queue_alloc(&dev.queue), "127 usable slots");
	zassert_is_null(ubt_queue_alloc(&dev.queue));
	zassert_equal(dev.queue.dropped, 2);

	/* The oldest packet comes out first and is not flagged. */
	const usb_pkt_rx *p = ubt_queue_peek(&dev.queue);

	zassert_equal(p->data[0], 0);
	zassert_equal(p->status & FIFO_OVERFLOW, 0);
	ubt_queue_pop(&dev.queue);

	/* A freed slot takes the next packet. */
	put(0xee);
	zassert_equal(ubt_queue_count(&dev.queue), UBT_QUEUE_SLOTS - 1U);
	zassert_is_null(ubt_queue_alloc(&dev.queue));
}

ZTEST(ubertooth_core, test_queue_overflow_status_lands_on_next_packet)
{
	for (unsigned int i = 0; i < UBT_QUEUE_SLOTS - 1U; i++) {
		put((uint8_t)i);
	}
	zassert_is_null(ubt_queue_alloc(&dev.queue));
	ubt_queue_pop(&dev.queue);
	put(0xaa);

	for (unsigned int i = 0; i < UBT_QUEUE_SLOTS - 2U; i++) {
		zassert_equal(ubt_queue_peek(&dev.queue)->status & FIFO_OVERFLOW, 0);
		ubt_queue_pop(&dev.queue);
	}
	const usb_pkt_rx *last = ubt_queue_peek(&dev.queue);

	zassert_equal(last->data[0], 0xaa);
	zassert_equal(last->status & FIFO_OVERFLOW, FIFO_OVERFLOW);
	ubt_queue_pop(&dev.queue);
	zassert_is_null(ubt_queue_peek(&dev.queue));
	zassert_equal(dev.queue.pending_status, 0);
}

ZTEST(ubertooth_core, test_queue_wraps_in_order)
{
	for (unsigned int round = 0; round < 5U; round++) {
		for (unsigned int i = 0; i < 100U; i++) {
			put((uint8_t)(round * 7U + i));
		}
		for (unsigned int i = 0; i < 100U; i++) {
			const usb_pkt_rx *p = ubt_queue_peek(&dev.queue);

			zassert_not_null(p);
			zassert_equal(p->data[0], (uint8_t)(round * 7U + i));
			ubt_queue_pop(&dev.queue);
		}
		zassert_is_null(ubt_queue_peek(&dev.queue));
	}
	put(1);
	ubt_queue_reset(&dev.queue);
	zassert_equal(ubt_queue_count(&dev.queue), 0);
}

/* ---- clock arithmetic ------------------------------------------------------------------ */

ZTEST(ubertooth_core, test_clock_follows_cycles)
{
	struct ubt_clock c;

	ubt_clock_init(&c, CYC_PER_100NS, 5000);
	zassert_equal(ubt_clock_t100(&c, 5000), 0);
	zassert_equal(ubt_clock_t100(&c, 5000 + 24), 1);
	zassert_equal(ubt_clock_t100(&c, 5000 + 24ULL * 3125), 3125);
	zassert_equal(ubt_clock_clkn(&c, 5000 + 75000ULL * 7), 7);
	/* Rebasing keeps the time. */
	ubt_clock_rebase(&c, 5000 + 75000ULL * 7);
	zassert_equal(ubt_clock_t100(&c, 5000 + 75000ULL * 7), 3125ULL * 7);
	zassert_equal(ubt_clock_t100(&c, 5000 + 75000ULL * 8), 3125ULL * 8);
}

ZTEST(ubertooth_core, test_clock_split_wraps)
{
	uint8_t high;
	uint32_t c100;

	ubt_clock_split(0, &high, &c100);
	zassert_equal(high, 0);
	zassert_equal(c100, 0);

	/* 2^20 CLKN ticks is the CLK100NS wrap and one step of clkn_high. */
	ubt_clock_split(3125ULL * 0xfffffU + 3124U, &high, &c100);
	zassert_equal(high, 0);
	zassert_equal(c100, 3125U * 0xfffffU + 3124U);
	zassert_equal(c100, UBT_CLK100NS_WRAP - 1U);
	ubt_clock_split(3125ULL * 0x100000U, &high, &c100);
	zassert_equal(high, 1);
	zassert_equal(c100, 0);
	ubt_clock_split(3125ULL * (0x1234567U) + 100U, &high, &c100);
	zassert_equal(high, 0x12);
	zassert_equal(c100, 3125U * 0x34567U + 100U);
	/* clkn_high keeps 8 bits of the 28 bit CLKN. */
	ubt_clock_split(3125ULL * 0xfffffffU, &high, &c100);
	zassert_equal(high, 0xff);
}

ZTEST(ubertooth_core, test_clock_set_keeps_phase)
{
	struct ubt_clock c;
	uint64_t t;

	ubt_clock_init(&c, CYC_PER_100NS, 0);
	t = 24ULL * 1000; /* 1000 units into the tick */
	ubt_clock_set_clkn(&c, t, 0x100);
	zassert_equal(ubt_clock_clkn(&c, t), 0x100);
	zassert_equal(ubt_clock_t100(&c, t), 3125ULL * 0x100 + 1000);
	zassert_equal(ubt_clock_t100(&c, t + 24ULL * 2125), 3125ULL * 0x101);
}

ZTEST(ubertooth_core, test_clock_trim_semantics)
{
	struct ubt_clock c;
	uint64_t t = 24ULL * 5000;

	ubt_clock_init(&c, CYC_PER_100NS, 0);

	/* 0..3124: the ticks are delayed, the clock reads behind by that much. */
	ubt_clock_trim(&c, t, 100);
	zassert_equal(ubt_clock_t100(&c, t), 5000 - 100);

	/* 3125..6249: advanced by 6250 - offset (the timer reload plus the extra two ticks). */
	ubt_clock_trim(&c, t, 6249);
	zassert_equal(ubt_clock_t100(&c, t), 5000 - 100 + 1);
	ubt_clock_trim(&c, t, 3125);
	zassert_equal(ubt_clock_t100(&c, t), 5000 - 100 + 1 + 3125);
}

ZTEST(ubertooth_core, test_clock_skew_start_hopping)
{
	struct ubt_clock c;

	ubt_clock_init(&c, CYC_PER_100NS, 0);
	ubt_clock_skew(&c, 0, 5, 50);
	zassert_equal(ubt_clock_t100(&c, 0), 3125ULL * 5 - 50);
}

ZTEST(ubertooth_core, test_clock_drift_sign)
{
	struct ubt_clock c;
	uint64_t one_second = 240000000ULL;

	ubt_clock_init(&c, CYC_PER_100NS, 0);
	zassert_equal(ubt_clock_t100(&c, one_second), 10000000ULL);

	/* A clock that is 100 ppm too fast is slowed down: 1 s reads 1 ms (10000 units) short... */
	ubt_clock_fix_drift(&c, 0, 100);
	zassert_equal(ubt_clock_t100(&c, one_second), 10000000ULL - 1000ULL);
	/* ...one that is too slow is sped up. */
	ubt_clock_fix_drift(&c, 0, -200);
	zassert_equal(c.drift_ppm, -100);
	zassert_equal(ubt_clock_t100(&c, one_second), 10000000ULL + 1000ULL);

	/* The sum saturates like upstream's int16. */
	ubt_clock_fix_drift(&c, 0, -32768);
	zassert_equal(c.drift_ppm, INT16_MIN);
	ubt_clock_fix_drift(&c, 0, 32767);
	ubt_clock_fix_drift(&c, 0, 32767);
	ubt_clock_fix_drift(&c, 0, 32767);
	zassert_equal(c.drift_ppm, INT16_MAX);
}

ZTEST(ubertooth_core, test_clock_monotonic_across_changes_and_inverse)
{
	struct ubt_clock c;
	uint64_t cyc = 1000;
	uint64_t last = 0;

	ubt_clock_init(&c, CYC_PER_100NS, cyc);
	for (unsigned int i = 0; i < 1000U; i++) {
		cyc += 24ULL * (100U + i % 7U);
		uint64_t t = ubt_clock_t100(&c, cyc);

		zassert_true(t >= last, "step %u", i);
		last = t;
		if (i % 50U == 0U) {
			ubt_clock_rebase(&c, cyc);
			zassert_equal(ubt_clock_t100(&c, cyc), last);
		}
		if (i % 120U == 0U) {
			ubt_clock_fix_drift(&c, cyc, (int16_t)(i % 3U) - 1);
			last = ubt_clock_t100(&c, cyc);
		}
	}

	/* cyc_at() inverts t100() to a cycle count within one unit. */
	for (int64_t drift = -300; drift <= 300; drift += 150) {
		ubt_clock_init(&c, CYC_PER_100NS, 777);
		ubt_clock_fix_drift(&c, 777, (int16_t)drift);
		uint64_t target = 123456789ULL;
		uint64_t at = ubt_clock_cyc_at(&c, target);
		uint64_t back = ubt_clock_t100(&c, at);

		zassert_true(back >= target - 1U && back <= target + 1U, "drift %lld: %llu vs %llu",
			     (long long)drift, (unsigned long long)back, (unsigned long long)target);
	}
}
