/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of the CPU-paced transmit (esp_sdr_paced.h): the argument
 * contract, the vendor tone and the stream excluding each other, a one-second stream at
 * 250 kS/s x4 with interrupts locked keeping its rate with no late write, missing input
 * counted, the TX DC register as it was after stop, and the receiver afterwards. The samples
 * are a quiet tone, 1000 of 32767 (an eighth of the S31's 16384 full scale): keep the power
 * low, nothing listens on purpose.
 */
#include <errno.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <esp_attr.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>

#if defined(CONFIG_SOC_SERIES_ESP32S31)
/* The TX DC value register (lib/esp_sdr/src/esp_sdr_priv.h): I in bits 9:0, Q in 19:10. */
#define TXDC_REG 0x20100c08U
/* In the S31's transmit range (2200 to 2900 MHz), away from the panel's PSRAM lines. */
#define TX_MHZ  2470U
/* Outside it: the first of these the receiver tunes must refuse to transmit. */
#define NO_TX_MHZ 2100U, 2150U, 2950U, 3000U
#define RX_RATE ESP_SDR_RATE_20MSPS
#else
#error "the TX DC register and frequencies for this SoC"
#endif

#define RATE_HZ 250000U
#define INTERP  4U
#define POWER   2U

/* One period of a tone at RATE_HZ / 16, in RAM for the IRAM callback. */
static int16_t tone_cos[16] = {1000, 924, 707, 383, 0, -383, -707, -924,
			       -1000, -924, -707, -383, 0, 383, 707, 924};

struct feed {
	uint32_t calls;
	bool give;
};

static IRAM_ATTR bool tone_input(void *user, int16_t *i, int16_t *q)
{
	struct feed *f = user;
	uint32_t k = f->calls++ & 15U;

	if (!f->give) {
		return false;
	}
	*i = tone_cos[k];
	*q = tone_cos[(k + 12U) & 15U];
	return true;
}

/* The stream for @p cycles CPU cycles, everything it runs in IRAM, interrupts locked. */
static IRAM_ATTR __noinline void run_locked(uint32_t cycles)
{
	unsigned int key = irq_lock();
	uint32_t c0;

	esp_sdr_paced_sync();
	c0 = esp_sdr_paced_cycles();
	while (esp_sdr_paced_cycles() - c0 < cycles) {
		(void)esp_sdr_tx_paced_poll();
	}
	irq_unlock(key);
}

/*
 * The floor's power: the third lowest of nine captures' mean square about their mean, in 1/256
 * of a raw unit squared. Wi-Fi frames only add power and may cover several of the 0.2 ms
 * captures; a receiver left in a wrong state raises all of them. All nine are printed.
 */
#define FLOOR_CAPTURES 9

static uint32_t floor_power(const char *tag)
{
	static struct esp_sdr_iq16 iq[4096];
	uint32_t p[FLOOR_CAPTURES];

	for (int c = 0; c < FLOOR_CAPTURES; c++) {
		int64_t si = 0, sq = 0, ss = 0, n = ARRAY_SIZE(iq);
		uint64_t t;

		zassert_ok(esp_sdr_rx_capture_iq(RX_RATE, ARRAY_SIZE(iq), iq, &t));
		for (size_t k = 0; k < ARRAY_SIZE(iq); k++) {
			si += iq[k].i;
			sq += iq[k].q;
			ss += (int32_t)iq[k].i * iq[k].i + (int32_t)iq[k].q * iq[k].q;
		}
		p[c] = (uint32_t)((256 * (ss * n - si * si - sq * sq)) / (n * n));
	}
	TC_PRINT("floor %s:", tag);
	for (int c = 0; c < FLOOR_CAPTURES; c++) {
		TC_PRINT(" %u", p[c]);
	}
	TC_PRINT("\n");
	for (int a = 0; a < FLOOR_CAPTURES; a++) {
		for (int b = a + 1; b < FLOOR_CAPTURES; b++) {
			if (p[b] < p[a]) {
				uint32_t t = p[a];

				p[a] = p[b];
				p[b] = t;
			}
		}
	}
	return p[2];
}

static void *suite_setup(void)
{
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	zassert_ok(esp_sdr_set_freq(TX_MHZ), "set_freq failed");
	return NULL;
}

static void after(void *f)
{
	ARG_UNUSED(f);
	esp_sdr_tx_paced_stop();
	esp_sdr_tx_tone_stop();
	(void)esp_sdr_set_freq(TX_MHZ);
}

ZTEST_SUITE(esp_sdr_tx_paced_api, NULL, suite_setup, NULL, after, NULL);

ZTEST(esp_sdr_tx_paced_api, test_rejects_bad_args)
{
	static const unsigned int no_tx[] = {NO_TX_MHZ};
	struct feed f = {.give = true};
	bool tuned = false;

	zassert_equal(esp_sdr_tx_paced_start(0, 1, POWER, tone_input, &f), -EINVAL, "rate 0");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 3, POWER, tone_input, &f), -EINVAL, "x3");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 16, POWER, tone_input, &f), -EINVAL, "x16");
	/* 48 kS/s: no whole number of CPU cycles a sample. */
	zassert_equal(esp_sdr_tx_paced_start(48000, 1, POWER, tone_input, &f), -EINVAL, "48 kS/s");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 1, esp_sdr_tx_paced_power_max() + 1U,
					     tone_input, &f), -EINVAL, "power past the maximum");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 1, POWER, NULL, &f), -EINVAL, "no input");
	ARRAY_FOR_EACH(no_tx, k) {
		if (esp_sdr_set_freq(no_tx[k]) == 0) {
			zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 1, POWER, tone_input, &f),
				      -ERANGE, "%u MHz", no_tx[k]);
			tuned = true;
			break;
		}
	}
	zassert_true(tuned, "none of the frequencies outside the transmit range tuned");
	zassert_ok(esp_sdr_set_freq(TX_MHZ));
	zassert_equal(f.calls, 0U, "the input ran on a refused start");
}

ZTEST(esp_sdr_tx_paced_api, test_tone_and_stream_exclude)
{
	struct feed f = {.give = true};

	zassert_ok(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX));
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f), -EBUSY,
		      "stream beside the tone");
	esp_sdr_tx_tone_stop();
	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	zassert_equal(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX), -EBUSY,
		      "tone beside the stream");
	esp_sdr_tx_paced_stop();
	/* Stop twice is a no-op; the tone works again after it. */
	esp_sdr_tx_paced_stop();
	zassert_ok(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX));
	esp_sdr_tx_tone_stop();
}

ZTEST(esp_sdr_tx_paced_api, test_one_second_locked)
{
	struct feed f = {.give = true};
	struct esp_sdr_paced_stats st;
	uint32_t before = sys_read32(TXDC_REG);

	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	run_locked(ESP_SDR_CPU_HZ);
	esp_sdr_paced_get_stats(&st);
	esp_sdr_tx_paced_stop();
	TC_PRINT("%u kS/s x%u: samples %llu late %u late_max %u cycles events %u\n",
		 RATE_HZ / 1000U, INTERP, (unsigned long long)st.samples, st.late, st.late_max,
		 st.events);
	zassert_true(st.samples >= RATE_HZ - 1U && st.samples <= RATE_HZ + 1U,
		     "%llu samples in 1 s", (unsigned long long)st.samples);
	zassert_equal(f.calls, (uint32_t)st.samples, "input calls");
	zassert_equal(st.late, 0U, "%u late writes, worst %u cycles", st.late, st.late_max);
	zassert_equal(st.events, 0U, "missing input");
	zassert_equal(sys_read32(TXDC_REG), before, "TX DC %08x, was %08x", sys_read32(TXDC_REG),
		      before);
}

/* Every rate the interpolator takes, briefly: each keeps its rate. */
ZTEST(esp_sdr_tx_paced_api, test_interp_rates)
{
	static const uint32_t rate[] = {250000, 250000, 250000, 125000, 125000, 125000};
	static const unsigned int interp[] = {1, 2, 4, 2, 4, 8};

	for (size_t k = 0; k < ARRAY_SIZE(rate); k++) {
		struct feed f = {.give = true};
		struct esp_sdr_paced_stats st;

		zassert_ok(esp_sdr_tx_paced_start(rate[k], interp[k], POWER, tone_input, &f));
		run_locked(ESP_SDR_CPU_HZ / 10U);
		esp_sdr_paced_get_stats(&st);
		esp_sdr_tx_paced_stop();
		TC_PRINT("%u kS/s x%u: samples %llu late %u late_max %u cycles\n", rate[k] / 1000U,
			 interp[k], (unsigned long long)st.samples, st.late, st.late_max);
		zassert_true(st.samples + 1U >= rate[k] / 10U && st.samples <= rate[k] / 10U + 1U,
			     "%u x%u: %llu samples in 100 ms", rate[k], interp[k],
			     (unsigned long long)st.samples);
		zassert_equal(st.late, 0U, "%u x%u: %u late, worst %u cycles", rate[k], interp[k],
			      st.late, st.late_max);
	}
}

/* An input that has nothing plays zeros and counts each of them. */
ZTEST(esp_sdr_tx_paced_api, test_missing_input_counted)
{
	struct feed f = {.give = false};
	struct esp_sdr_paced_stats st;

	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	run_locked(ESP_SDR_CPU_HZ / 100U);
	esp_sdr_paced_get_stats(&st);
	esp_sdr_tx_paced_stop();
	zassert_true(st.samples > 0U);
	zassert_equal(st.events, (uint32_t)st.samples, "events %u of %llu", st.events,
		      (unsigned long long)st.samples);
}

/*
 * After a stream the radio receives as before, at a fixed gain: the floor's power within 3 dB
 * of before (over the air two sets before a stream differ by up to 1.4 dB; a receiver left
 * transmitting or at another gain is off by far more).
 */
ZTEST(esp_sdr_tx_paced_api, test_receive_after)
{
	struct feed f = {.give = true};
	uint32_t before, after_p;

	zassert_ok(esp_sdr_rx_set_gain(40));
	before = floor_power("before");
	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	run_locked(ESP_SDR_CPU_HZ / 10U);
	esp_sdr_tx_paced_stop();
	after_p = floor_power("after");
	zassert_ok(esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO));
	TC_PRINT("floor power %u before, %u after (1/256 unit^2)\n", before, after_p);
	zassert_true(before > 0U, "a dead receiver before");
	zassert_true(after_p <= 2U * before && 2U * after_p >= before, "power %u after, %u before",
		     after_p, before);
}
