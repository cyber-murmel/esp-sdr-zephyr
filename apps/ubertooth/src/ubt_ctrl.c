/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_ctrl.h. Request semantics follow upstream's vendor_request_handler();
 * deviations are marked "upstream bug fixed" or "not supported".
 */

#include <errno.h>
#include <string.h>

#include "ubt_ctrl.h"

/* BTLE_SET_TARGET mask length limit (bits of the 48 bit address). */
#define TARGET_BITS_MAX 48U

bool ubt_ctrl_known(uint8_t request)
{
	return request <= UBT_REQ_MAX && request != 64U;
}

static uint64_t now(const struct ubt_dev *d)
{
	return d->plat->cycles();
}

static void kick(const struct ubt_dev *d)
{
	if (d->plat->kick != NULL) {
		d->plat->kick();
	}
}

static int need(size_t len, size_t n)
{
	return len >= n ? 0 : -EINVAL;
}

/* Ask for @p mode if the radio code has it. */
static int start_mode(struct ubt_dev *d, uint8_t mode)
{
	if ((d->supported_modes & UBT_MODE_BIT(mode)) == 0U) {
		return -ENOTSUP;
	}
	d->requested_mode = mode;
	kick(d);
	return 0;
}

static int reply_byte(uint8_t *data, size_t len, uint8_t v)
{
	if (len < 1U) {
		return -EINVAL;
	}
	data[0] = v;
	return 1;
}

static int reply_u32(uint8_t *data, size_t len, uint32_t v)
{
	if (len < 4U) {
		return -EINVAL;
	}
	for (unsigned int i = 0; i < 4U; i++) {
		data[i] = (uint8_t)(v >> (8U * i));
	}
	return 4;
}

static int set_led(struct ubt_dev *d, unsigned int led, uint16_t value)
{
	d->leds[led] = value != 0U;
	if (d->plat->led != NULL) {
		d->plat->led(led, d->leds[led]);
	}
	return 0;
}

static int get_led(const struct ubt_dev *d, unsigned int led, uint8_t *data, size_t len)
{
	return reply_byte(data, len, d->leds[led] ? 1U : 0U);
}

static int set_channel(struct ubt_dev *d, uint16_t value)
{
	uint16_t ch = value;

	if (ch > UBT_MAX_FREQ) {
		/* The host sends 9999 for the band sweep. */
		d->hop_mode = HOP_SWEEP;
		ch = 2402;
	} else {
		d->hop_mode = HOP_NONE;
		ch = ch < UBT_MIN_FREQ ? UBT_MIN_FREQ : ch;
		ch = ch > UBT_MAX_FREQ ? UBT_MAX_FREQ : ch;
	}
	d->requested_channel = ch;
	d->le_adv_channel = ch;
	if (d->mode != MODE_BT_FOLLOW_LE) {
		d->channel = ch;
		d->requested_channel = 0;
	}
	d->cfg_gen++;
	kick(d);
	return 0;
}

static int set_target(struct ubt_dev *d, const uint8_t *data, size_t len)
{
	struct ubt_le_target *t = &d->le.target;
	unsigned int bits;
	unsigned int i;
	unsigned int last;

	if (need(len, 7U) != 0) {
		return -EINVAL;
	}
	bits = data[6];
	if (bits > TARGET_BITS_MAX) {
		return -EINVAL;
	}
	memset(t, 0, sizeof(*t));
	if (bits == 0U) {
		return 0;
	}
	/* Addresses appear in packets in reverse octet order; store them that way. */
	for (i = 0; i < 6U; i++) {
		t->addr[i] = data[5U - i];
	}
	for (i = 5; bits > 8U; i--, bits -= 8U) {
		t->mask[i] = 0xff;
	}
	last = i;
	t->mask[last] = (uint8_t)(0xffU << (8U - bits));
	/* Upstream clears only the first five octets here, leaving stray bits in the sixth (fixed). */
	for (i = 0; i < 6U; i++) {
		t->addr[i] &= t->mask[i];
	}
	t->set = true;
	return 0;
}

static int reply_string(uint8_t *data, size_t len, const char *s, size_t lead)
{
	size_t n = strlen(s);

	if (len < lead + 1U) {
		return -EINVAL;
	}
	if (n > len - lead - 1U) {
		n = len - lead - 1U;
	}
	if (n > 255U) {
		n = 255U;
	}
	memset(data, 0, lead);
	data[lead] = (uint8_t)n;
	memcpy(&data[lead + 1U], s, n);
	return (int)(lead + 1U + n);
}

int ubt_ctrl(struct ubt_dev *d, const struct ubt_req *r, uint8_t *data, size_t len)
{
	const struct ubt_platform *p = d->plat;
	uint16_t value = r->value;
	int ret;

	switch (r->request) {
	case UBERTOOTH_PING:
		return 0;

	case UBERTOOTH_RX_SYMBOLS:
		return start_mode(d, MODE_RX_SYMBOLS);

	case UBERTOOTH_TX_SYMBOLS:
		/* Upstream also forces Bluetooth hopping for this mode. */
		ret = start_mode(d, MODE_TX_SYMBOLS);
		if (ret == 0) {
			d->hop_mode = HOP_BLUETOOTH;
		}
		return ret;

	case UBERTOOTH_GET_USRLED:
		return get_led(d, 0, data, len);
	case UBERTOOTH_SET_USRLED:
		return set_led(d, 0, value);
	case UBERTOOTH_GET_RXLED:
		return get_led(d, 1, data, len);
	case UBERTOOTH_SET_RXLED:
		return set_led(d, 1, value);
	case UBERTOOTH_GET_TXLED:
		return get_led(d, 2, data, len);
	case UBERTOOTH_SET_TXLED:
		return set_led(d, 2, value);

	case UBERTOOTH_GET_1V8:
		return reply_byte(data, len, d->cc1v8 ? 1U : 0U);
	case UBERTOOTH_SET_1V8:
		d->cc1v8 = value != 0U;
		return 0;

	case UBERTOOTH_GET_CHANNEL:
		if (len < 2U) {
			return -EINVAL;
		}
		data[0] = (uint8_t)d->channel;
		data[1] = (uint8_t)(d->channel >> 8);
		return 2;
	case UBERTOOTH_SET_CHANNEL:
		return set_channel(d, value);

	case UBERTOOTH_RESET:
		d->reset_dfu = false;
		d->requested_mode = MODE_RESET;
		kick(d);
		return 0;

	case UBERTOOTH_GET_SERIAL:
		if (len < 17U) {
			return -EINVAL;
		}
		memset(data, 0, 17U);
		if (p->serial != NULL) {
			p->serial(&data[1]);
		}
		return 17;

	case UBERTOOTH_GET_PARTNUM:
		if (len < 5U) {
			return -EINVAL;
		}
		data[0] = 0;
		(void)reply_u32(&data[1], len - 1U, p->part_num);
		return 5;

	case UBERTOOTH_GET_PAEN:
		return reply_byte(data, len, d->paen ? 1U : 0U);
	case UBERTOOTH_SET_PAEN:
		d->paen = value != 0U;
		d->cfg_gen++;
		return 0;
	case UBERTOOTH_GET_HGM:
		return reply_byte(data, len, d->hgm ? 1U : 0U);
	case UBERTOOTH_SET_HGM:
		d->hgm = value != 0U;
		d->cfg_gen++;
		return 0;

	case UBERTOOTH_TX_TEST:
		return start_mode(d, MODE_TX_TEST);

	case UBERTOOTH_GET_PALEVEL:
		return reply_byte(data, len, d->pa_level & 7U);
	case UBERTOOTH_SET_PALEVEL:
		if (value >= 8U) {
			return -EINVAL;
		}
		d->pa_level = (uint8_t)value;
		d->cfg_gen++;
		return 0;

	case UBERTOOTH_RANGE_TEST:
		return start_mode(d, MODE_RANGE_TEST);
	case UBERTOOTH_REPEATER:
		return start_mode(d, MODE_REPEATER);

	case UBERTOOTH_RANGE_CHECK:
		if (len < 5U) {
			return -EINVAL;
		}
		data[0] = d->rr.valid;
		data[1] = d->rr.request_pa;
		data[2] = d->rr.request_num;
		data[3] = d->rr.reply_pa;
		data[4] = d->rr.reply_num;
		return 5;

	case UBERTOOTH_STOP:
		d->requested_mode = MODE_IDLE;
		kick(d);
		return 0;

	case UBERTOOTH_GET_MOD:
		return reply_byte(data, len, d->modulation);
	case UBERTOOTH_SET_MOD:
		d->modulation = (uint8_t)value;
		d->cfg_gen++;
		return 0;

	case UBERTOOTH_SET_ISP:
		/* The LPC ISP loader does not exist here. */
		return -ENOTSUP;

	case UBERTOOTH_FLASH:
		if (p->reset == NULL) {
			return -ENOTSUP;
		}
		d->reset_dfu = true;
		d->requested_mode = MODE_RESET;
		kick(d);
		return 0;

	case UBERTOOTH_SPECAN:
		if (value < UBT_SPECAN_MIN || value > UBT_SPECAN_MAX || r->index < UBT_SPECAN_MIN ||
		    r->index > UBT_SPECAN_MAX || r->index < value) {
			return -EINVAL;
		}
		ret = start_mode(d, MODE_SPECAN);
		if (ret == 0) {
			d->low_freq = value;
			d->high_freq = r->index;
			d->cfg_gen++;
		}
		return ret;

	case UBERTOOTH_RX_GENERIC:
		return start_mode(d, MODE_RX_GENERIC);

	case UBERTOOTH_LED_SPECAN:
		if (value > 256U) {
			return -EINVAL;
		}
		ret = start_mode(d, MODE_LED_SPECAN);
		if (ret == 0) {
			d->rssi_threshold = (int8_t)(UBT_RSSI_OFFSET - (int)value);
		}
		return ret;

	case UBERTOOTH_GET_REV_NUM:
		return reply_string(data, len, p->rev, 2U);
	case UBERTOOTH_GET_COMPILE_INFO:
		return reply_string(data, len, p->compile_info, 0U);

	case UBERTOOTH_GET_BOARD_ID:
		return reply_byte(data, len, UBT_BOARD_ID);

	case UBERTOOTH_SET_SQUELCH:
		d->cs_threshold_req = (int8_t)value;
		d->cfg_gen++;
		return 0;
	case UBERTOOTH_GET_SQUELCH:
		return reply_byte(data, len, (uint8_t)d->cs_threshold_req);

	case UBERTOOTH_SET_BDADDR:
		if (need(len, 16U) != 0) {
			return -EINVAL;
		}
		d->target.address = 0;
		d->target.syncword = 0;
		for (unsigned int i = 0; i < 8U; i++) {
			d->target.address |= (uint64_t)data[i] << (8U * i);
			d->target.syncword |= (uint64_t)data[i + 8U] << (8U * i);
		}
		d->cfg_gen++;
		return 0;

	case UBERTOOTH_START_HOPPING:
		if (need(len, 6U) != 0) {
			return -EINVAL;
		}
		ret = start_mode(d, MODE_BT_FOLLOW);
		if (ret == 0) {
			uint32_t clkn_offset = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
					       ((uint32_t)data[2] << 8) | data[3];

			ubt_clock_skew(&d->clock, now(d), clkn_offset,
				       (uint16_t)(((uint16_t)data[4] << 8) | data[5]));
			d->hop_mode = HOP_BLUETOOTH;
		}
		return ret;

	case UBERTOOTH_AFH:
		ret = start_mode(d, MODE_AFH);
		if (ret == 0) {
			d->hop_mode = HOP_AFH;
			memset(d->afh_map, 0, sizeof(d->afh_map));
			d->afh_enabled = true;
		}
		return ret;

	case UBERTOOTH_HOP:
		d->do_hop = 1;
		kick(d);
		return 0;

	case UBERTOOTH_SET_CLOCK:
		if (need(len, 4U) != 0) {
			return -EINVAL;
		}
		ubt_clock_set_clkn(&d->clock, now(d),
				   (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
					   ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
		d->cfg_gen++;
		return 0;
	case UBERTOOTH_GET_CLOCK:
		return reply_u32(data, len, ubt_clock_clkn(&d->clock, now(d)));

	case UBERTOOTH_SET_AFHMAP:
		if (need(len, 10U) != 0) {
			return -EINVAL;
		}
		memcpy(d->afh_map, data, 10U);
		d->afh_enabled = true;
		return 0;
	case UBERTOOTH_CLEAR_AFHMAP:
		memset(d->afh_map, 0, sizeof(d->afh_map));
		d->afh_enabled = false;
		return 0;

	case UBERTOOTH_TRIM_CLOCK:
		if (need(len, 2U) != 0) {
			return -EINVAL;
		}
		ubt_clock_trim(&d->clock, now(d), (uint16_t)(((uint16_t)data[0] << 8) | data[1]));
		return 0;

	case UBERTOOTH_FIX_CLOCK_DRIFT:
		if (need(len, 2U) != 0) {
			return -EINVAL;
		}
		ubt_clock_fix_drift(&d->clock, now(d), (int16_t)(((uint16_t)data[0] << 8) | data[1]));
		return 0;

	case UBERTOOTH_BTLE_SNIFFING:
		ret = start_mode(d, MODE_BT_FOLLOW_LE);
		if (ret == 0) {
			d->le.follow = value != 0U;
			d->do_hop = 0;
			d->hop_mode = HOP_BTLE;
			d->cfg_gen++;
		}
		return ret;

	case UBERTOOTH_GET_ACCESS_ADDRESS:
		return reply_u32(data, len, d->le.access_address);
	case UBERTOOTH_SET_ACCESS_ADDRESS:
		if (need(len, 4U) != 0) {
			return -EINVAL;
		}
		d->le.access_address = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
				       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
		d->le.target.set = true;
		d->cfg_gen++;
		return 0;

	case UBERTOOTH_DO_SOMETHING:
		return 0;
	case UBERTOOTH_DO_SOMETHING_REPLY:
		if (len < 2U) {
			return -EINVAL;
		}
		data[0] = 0x13;
		data[1] = 0x37;
		return 2;

	case UBERTOOTH_GET_CRC_VERIFY:
		return reply_byte(data, len, d->le.crc_verify ? 1U : 0U);
	case UBERTOOTH_SET_CRC_VERIFY:
		d->le.crc_verify = value != 0U;
		d->cfg_gen++;
		return 0;

	case UBERTOOTH_POLL: {
		usb_pkt_rx pkt;

		if (len >= sizeof(pkt) && ubt_queue_take(&d->queue, &pkt)) {
			memcpy(data, &pkt, sizeof(pkt));
			return (int)sizeof(pkt);
		}
		return reply_byte(data, len, 0);
	}

	case UBERTOOTH_BTLE_PROMISC:
		ret = start_mode(d, MODE_BT_PROMISC_LE);
		if (ret == 0) {
			d->hop_mode = HOP_NONE;
			d->cfg_gen++;
		}
		return ret;

	case UBERTOOTH_READ_REGISTER: {
		uint16_t v = ubt_regs_read(&d->regs, (uint8_t)value);

		if (len < 2U) {
			return -EINVAL;
		}
		data[0] = (uint8_t)(v >> 8);
		data[1] = (uint8_t)v;
		return 2;
	}
	case UBERTOOTH_WRITE_REGISTER:
		ubt_regs_write(&d->regs, (uint8_t)(value & 0xffU), r->index);
		return 0;
	case UBERTOOTH_WRITE_REGISTERS:
		if (need(len, 3U * (size_t)value) != 0) {
			return -EINVAL;
		}
		for (unsigned int i = 0; i < value; i++) {
			ubt_regs_write(&d->regs, data[3U * i],
				       (uint16_t)(((uint16_t)data[3U * i + 1U] << 8) | data[3U * i + 2U]));
		}
		return 0;
	case UBERTOOTH_READ_ALL_REGISTERS:
		return (int)ubt_regs_dump(&d->regs, data, len);

	case UBERTOOTH_TX_GENERIC_PACKET:
		/*
		 * Upstream copies 7 + length bytes, which loses the last data byte and the pa_level
		 * field's neighbour; take everything the host sent (fixed).
		 */
		if (need(len, 8U) != 0 || len > sizeof(d->tx_pkt)) {
			return -EINVAL;
		}
		ret = start_mode(d, MODE_TX_GENERIC);
		if (ret == 0) {
			memcpy(&d->tx_pkt, data, len);
		}
		return ret;

	case UBERTOOTH_BTLE_SLAVE:
		if (need(len, 6U) != 0) {
			return -EINVAL;
		}
		ret = start_mode(d, MODE_BT_SLAVE_LE);
		if (ret == 0) {
			memcpy(d->slave_mac, data, 6U);
		}
		return ret;

	case UBERTOOTH_LE_SET_ADV_DATA:
		if (len > LE_ADV_MAX_LEN) {
			return -EINVAL;
		}
		d->adv_len = (uint16_t)len;
		memcpy(d->adv_data, data, len);
		return 0;

	case UBERTOOTH_BTLE_SET_TARGET:
		return set_target(d, data, len);

	case UBERTOOTH_CANCEL_FOLLOW:
		d->cancel_follow = true;
		kick(d);
		return 0;

	case UBERTOOTH_JAM_MODE:
		/* Jamming needs a reaction inside the packet: not supported, so the tool says so. */
		if (value != JAM_NONE) {
			return -ENOTSUP;
		}
		d->jam_mode = JAM_NONE;
		return 0;

	case UBERTOOTH_EGO:
		d->ego_mode = (uint8_t)value;
		return start_mode(d, MODE_EGO);

	case UBERTOOTH_RFCAT_SUBCMD:
		return -ENOTSUP;

	case UBERTOOTH_XMAS:
		return start_mode(d, MODE_XMAS);

	default:
		/* BOOTLOADER_FLASH, BTLE_PHY and the numbers upstream leaves unhandled. */
		return -ENOTSUP;
	}
}
