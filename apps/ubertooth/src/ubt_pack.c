/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ring sink's samples to the sample ring's 8 bits (ubt_radio.c): on the vector unit
 * (ubt_pack8.S) once a check at boot found it equal to the C below, else in C. The check runs
 * from flash: the flash code CPU 0 runs hot has a place of its own (hot_text_s3.ld), which flash
 * code added elsewhere no longer moves (doc/apps/ubertooth-internals.md). ubt_radio.c's pack()
 * keeps its C loop, starting where ubt_pack() stopped.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_attr.h>
#include <esp_sdr/esp_sdr_xtensa.h>

#include "ubt_attr.h"
#include "ubt_pack.h"

LOG_MODULE_REGISTER(ubt_pack, LOG_LEVEL_INF);

/* ubt_pack8.S: pack_c() on the vector unit; k = {1, 127}. */
void ubt_pack8(int8_t *di, int8_t *dq, const int16_t *i, const int16_t *q, uint32_t n,
	       const int16_t *k);

static const int16_t pack_k[2] __aligned(4) ESP_SDR_DATA(TABLE, pack_k) = {1, 127};
static bool pack_vec ESP_SDR_DATA(STATE, pack_vec);

/* x >> 6 up to 127 (the -fs/4 mix can make +8192, one past int8 after the shift). */
ESP_SDR_CODE(COLD, pack_c)
static void pack_c(int8_t *di, int8_t *dq, const int16_t *i, const int16_t *q, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		di[k] = (int8_t)MIN(i[k] >> UBT_PACK_SHIFT, 127);
		dq[k] = (int8_t)MIN(q[k] >> UBT_PACK_SHIFT, 127);
	}
}

UBT_HOT(PACED_SAMPLE, ubt_pack)
size_t ubt_pack(int8_t *di, int8_t *dq, const int16_t *i, const int16_t *q, size_t n)
{
	if (!pack_vec) {
		return 0;
	}
	ubt_pack8(di, dq, i, q, n, pack_k);
	return n;
}

/*
 * At boot: random samples at every alignment of the ring slot and of the source, lengths 0 to 99,
 * over the int16 range and the stream's +-8192, the bytes around the output included. The vector
 * unit only if all 1600 cases agree.
 */
ESP_SDR_CODE(COLD, pack_selftest)
static int pack_selftest(void)
{
	/* PSRAM: internal RAM is short, and the check is not timed. */
	static ESP_SDR_DATA(BULK, si) int16_t si[160] __aligned(16);
	static ESP_SDR_DATA(BULK, sq) int16_t sq[160] __aligned(16);
	static ESP_SDR_DATA(BULK, a) int8_t a[2][144] __aligned(16);
	static ESP_SDR_DATA(BULK, b) int8_t b[2][144] __aligned(16);
	uint32_t x = k_cycle_get_32() | 1U;
	uint32_t bad = 0;

	for (uint32_t c = 0; c < 1600U; c++) {
		size_t n = c % 100U, off = (c / 100U) % 16U, src = (c / 7U) % 8U;
		unsigned int key;

		for (size_t k = 0; k < ARRAY_SIZE(si); k++) {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			si[k] = (int16_t)x;
			/* Every other case within the stream's +-8192. */
			sq[k] = (c & 1U) != 0U ? (int16_t)(x >> 16) :
						 (int16_t)(((x >> 16) % 16385U) - 8192U);
		}
		memset(a, 0x5a, sizeof(a));
		memset(b, 0x5a, sizeof(b));
		pack_c(&a[0][16 + off], &a[1][16 + off], &si[src], &sq[src], n);
		key = arch_irq_lock();
		esp_sdr_pie_enable();
		ubt_pack8(&b[0][16 + off], &b[1][16 + off], &si[src], &sq[src], n, pack_k);
		arch_irq_unlock(key);
		bad += memcmp(a, b, sizeof(a)) != 0 ? 1U : 0U;
	}
	pack_vec = bad == 0U;
	if (bad != 0U) {
		LOG_ERR("ring sink's pack on the vector unit: %u of 1600 cases differ, C instead",
			bad);
	} else {
		LOG_INF("ring sink's pack on the vector unit: 1600 cases as C");
	}
	return 0;
}

SYS_INIT(pack_selftest, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
