/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Device state: what the vendor requests set and the mode threads read (the
 * globals of upstream's bluetooth_rxtx.c). The USB thread writes, the mode
 * thread reads, as upstream does with its volatile globals; multi byte
 * blocks (target, advertising data) are only read after a mode start.
 */

#ifndef UBT_DEV_H_
#define UBT_DEV_H_

#include <stdbool.h>
#include <stdint.h>

#include "ubt_clock.h"
#include "ubt_proto.h"
#include "ubt_queue.h"
#include "ubt_regs.h"

#define UBT_MODE_BIT(m) (1UL << (m))

/* Hooks into the platform (LEDs, reset, ...); any may be NULL. */
struct ubt_platform {
	/* LED 0 USR, 1 RX, 2 TX. */
	void (*led)(unsigned int led, bool on);
	/* Wake the mode thread after a request that it has to act on. */
	void (*kick)(void);
	/* After the reply of RESET (dfu = false) or FLASH (dfu = true) went out. Returns false if impossible. */
	bool (*reset)(bool dfu);
	/* The 16 byte serial number GET_SERIAL reports. */
	void (*serial)(uint8_t serial[16]);
	/* The CPU cycle counter for the clock. */
	uint64_t (*cycles)(void);
	/* GET_REV_NUM and GET_COMPILE_INFO strings. */
	const char *rev;
	const char *compile_info;
	uint32_t part_num;
};

struct ubt_le_target {
	bool set;
	/* In over the air (reversed) order, as upstream stores them. */
	uint8_t addr[6];
	uint8_t mask[6];
};

struct ubt_dev {
	const struct ubt_platform *plat;
	struct ubt_clock clock;
	struct ubt_queue queue;
	struct ubt_regs regs;

	/* Modes the radio code implements; a request for another stalls. */
	uint32_t supported_modes;

	volatile uint8_t mode;
	volatile uint8_t requested_mode;
	volatile uint8_t hop_mode;
	volatile uint8_t do_hop;
	/* RESET (false) or FLASH (true) is pending: the mode thread resets after the reply went out. */
	volatile bool reset_dfu;
	volatile uint16_t channel;
	volatile uint16_t requested_channel;
	volatile uint16_t le_adv_channel;
	/* Incremented whenever the channel, squelch or another setting the radio code derives from changes. */
	volatile uint32_t cfg_gen;
	volatile uint8_t modulation;

	uint16_t low_freq;
	uint16_t high_freq;
	int8_t rssi_threshold;
	uint8_t jam_mode;
	uint8_t ego_mode;

	/* SET_SQUELCH as the host sent it (dBm, or relative to the average when > 0). */
	int8_t cs_threshold_req;

	struct {
		uint32_t access_address;
		bool crc_verify;
		bool follow;
		struct ubt_le_target target;
	} le;
	volatile bool cancel_follow;

	uint8_t slave_mac[6];
	uint8_t adv_data[LE_ADV_MAX_LEN];
	uint16_t adv_len;

	bdaddr target;
	uint8_t afh_map[10];
	bool afh_enabled;
	generic_tx_packet tx_pkt;
	rangetest_result rr;

	bool leds[3];
	bool cc1v8;
	bool paen;
	bool hgm;
	uint8_t pa_level;
};

/* Defaults, as after upstream's cc2400_idle(). The queue slots must be set up separately. */
void ubt_dev_reset(struct ubt_dev *d);

#endif /* UBT_DEV_H_ */
