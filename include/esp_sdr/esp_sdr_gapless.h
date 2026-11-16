/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR gapless receive (ESP32-C6): the whole 80 MS/s, pruned to what a link carries.
 *
 * The dump engine runs as a circular ring of ESP_SDR_GAPLESS_RING samples and
 * never stops, while the CPU switches it between SRAM blocks 1 and 2 every
 * ESP_SDR_GAPLESS_UNIT samples (upstream ESPARGOS ring_capture.c, after
 * h0m3us3r/eSpDR). Before each switch the CPU writes marker words into the
 * idle block where the next unit will start and end; after it, it finds the
 * exact first and last sample there, and every unit must start where the last
 * one ended: the boundaries are proven gapless. The CPUs cannot reach a block
 * while the engine writes it, so a unit is read from its block during the
 * next unit, until the block is prepared for the one after.
 *
 * Meanwhile the IQ estimator sums the same input: one reading every
 * ESP_SDR_GAPLESS_WINDOW_US gives each window's power about its mean, on the
 * ring's own index axis. Windows past a threshold over the quiet baseline are
 * kept, with windows before and after them: copied raw into a staging ring
 * while their block can still be read, then packed to int8 I/Q (a right shift
 * chosen from the window's power and DC, the DC left in and reported) into the
 * caller's sink, which sends them on. Each segment keeps its index on the
 * run's axis (80 MS/s, 40 with esp_sdr_gapless_cfg.half_rate).
 *
 * Two servos keep headroom: the threshold rises and falls so that what is kept
 * fills the link's budget short of its headroom, and the receive gain steps so
 * that the loudest windows' estimated peaks stay short of full scale by theirs,
 * without lifting the noise floor further than it needs.
 *
 * With width sorting (esp_sdr_gapless_cfg.wide) the kept samples are judged
 * by how fast they decorrelate before they are packed: a chunk of a window's
 * length whose correlations 4, 6 and 10 samples (50 to 125 ns) apart all stay
 * below half its power over the noise carries a signal wider than some
 * 10 MHz (Wi-Fi), and goes to the sink as a short record
 * (ESP_SDR_GAPLESS_SEG_WIDE) instead of its samples, so the link's budget goes
 * to the narrow ones and the threshold servo can hold a lower threshold.
 *
 * A run holds interrupts off on the CPU from start to end: the sink's
 * service() callback is where the caller moves data and polls its link.
 */

#ifndef ESP_SDR_ESP_SDR_GAPLESS_H_
#define ESP_SDR_ESP_SDR_GAPLESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_attr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_gapless
 * @{
 */

/**
 * Code on the run's path (upstream's RING_HOT): in IRAM, since with interrupts off a flash
 * cache miss costs hundreds of cycles against the ring's deadlines, and optimized as hot.
 * The sink's callbacks need it too.
 */
#define ESP_SDR_GAPLESS_HOT IRAM_ATTR __attribute__((hot))

/** Samples in the ring (one lap, 205 us at 80 MS/s, 410 us at 40). */
#define ESP_SDR_GAPLESS_RING 16384U
/** Samples per unit: the CPU switches blocks after this many (154 us, 307 at 40 MS/s). */
#define ESP_SDR_GAPLESS_UNIT 12288U
/** Sample rate (esp_sdr_gapless_cfg.half_rate: half of it). */
#define ESP_SDR_GAPLESS_RATE_HZ 80000000U
/** Estimator window, the unit of the keep decision. */
#define ESP_SDR_GAPLESS_WINDOW_US 10U

/** Why a run ended early. */
enum esp_sdr_gapless_error {
	ESP_SDR_GAPLESS_OK = 0,
	/** A block was owned by the engine too long (its ring lapped). */
	ESP_SDR_GAPLESS_ERR_AGE,
	/** The switch came too late after the threshold. */
	ESP_SDR_GAPLESS_ERR_LATE,
	/** A unit did not start where the last one ended. */
	ESP_SDR_GAPLESS_ERR_START,
	/** A unit's end was not found. */
	ESP_SDR_GAPLESS_ERR_END,
	/** A unit's length was out of range. */
	ESP_SDR_GAPLESS_ERR_LENGTH,
};

/** Segment flags (the same values as esdr_proto.h's ESDR_SEG_*). */
#define ESP_SDR_GAPLESS_SEG_DROPPED 0x01U
#define ESP_SDR_GAPLESS_SEG_RESTART 0x02U
#define ESP_SDR_GAPLESS_SEG_LOUD    0x04U
/**
 * A width record (esp_sdr_gapless_cfg.wide): the segment's ESP_SDR_GAPLESS_WIDE_PAIRS pairs hold
 * a struct esp_sdr_gapless_wide, not samples; the samples it stands for (its span, from the
 * segment's index on) carried a signal wider than some 10 MHz and were not sent.
 */
#define ESP_SDR_GAPLESS_SEG_WIDE    0x08U

/** Pairs of a width record's segment: the bytes of struct esp_sdr_gapless_wide. */
#define ESP_SDR_GAPLESS_WIDE_PAIRS 4U

/**
 * A width record's payload (esdr_proto.h's struct esdr_seg_wide, the same bytes). The
 * correlations are over the record's samples less their DC, summed at evenly spaced points
 * (points of them).
 */
struct esp_sdr_gapless_wide {
	/** Samples the record stands for, from the segment's index on. */
	uint16_t span;
	/**
	 * The correlation one sample apart, scaled so that the larger component is +-127: its
	 * angle is the signal's mean frequency in radians a sample (80 MHz a turn).
	 */
	int8_t r1_i, r1_q;
	/** Correlations 4, 6 and 10 samples apart over the power above the noise, 255 for 1. */
	uint8_t rho4, rho6, rho10;
	/** Points the correlations summed. */
	uint8_t points;
} __attribute__((packed));

/** A segment of kept samples, as reserve() is asked for it. */
struct esp_sdr_gapless_seg {
	/** First sample's index on the run's axis (80 or 40 MS/s), and up to how many samples. */
	uint64_t index;
	uint32_t pairs;
	/** int8 = raw >> shift (saturated); the DC is left in. */
	uint8_t shift;
	/** ESP_SDR_GAPLESS_SEG_*. */
	uint8_t flags;
	/** Receive gain index the samples were taken at. */
	uint8_t gain;
	/** The receiver's DC, raw units (subtract it from int8 << shift). */
	int8_t dc_i, dc_q;
	/** Power of the segment's first window, dB below full scale, times 10. */
	int16_t power_dbfs_x10;
};

/**
 * Where kept samples go. Every callback runs with interrupts off, between the ring's
 * deadlines: mark them ESP_SDR_GAPLESS_HOT, with what they call.
 */
struct esp_sdr_gapless_sink {
	/**
	 * Room for segment @p seg: return how many of its samples fit (0: none for now; the
	 * samples wait in their block and are lost if it is needed again first) and where they
	 * go (2 bytes each, I then Q). The library writes them, then calls commit().
	 */
	uint32_t (*reserve)(const struct esp_sdr_gapless_seg *seg, int8_t **dst, void *user);
	/** The samples of the last reserve() are written; @p pairs of them (at most the granted). */
	void (*commit)(uint32_t pairs, void *user);
	/**
	 * Free time: move data, poll the link. Must return within @p budget CPU cycles; returns
	 * true if it did something (the library then gives it the next free time again).
	 */
	bool (*service)(uint32_t budget, void *user);
	/** Polled once per unit: true ends the run. */
	bool (*stop)(void *user);
	void *user;
};

/** What to keep. */
struct esp_sdr_gapless_cfg {
	/**
	 * Lowest threshold over the quiet baseline in sixteenths (0: 64, 6 dB), also what
	 * counts as quiet for the baseline; ignored with @p level.
	 */
	uint16_t ratio_x16;
	/** Windows kept before and after a window past the threshold (up to 4). */
	uint8_t pre, post;
	/** Fixed threshold, window power per sample in raw units squared (0: from the baseline). */
	uint32_t level;
	/**
	 * Payload bytes per second the sink can send: the threshold servo keeps what is kept
	 * between (100 - headroom_hi) and (100 - headroom_lo) percent of it. 0: no servo.
	 */
	uint32_t budget_bps;
	/** Headroom band in percent, for both servos (0, 0: 5 and 10). */
	uint8_t headroom_lo, headroom_hi;
	/** Step the receive gain to keep the peaks in the headroom band. */
	bool gain_servo;
	/** Gain index range of the servo (0, 0: the PHY's whole range). */
	uint8_t gain_min, gain_max;
	/**
	 * Width sorting: chunks of kept samples that carry a signal wider than some 10 MHz go to
	 * the sink as width records (ESP_SDR_GAPLESS_SEG_WIDE) instead of their samples. The sink's
	 * reserve() must then grant a width record's ESP_SDR_GAPLESS_WIDE_PAIRS pairs whole, as a
	 * segment of its own, or none. Off with @p half_rate.
	 */
	bool wide;
	/**
	 * The dump engine at 40 MS/s (its own rate bit): every index, count and length of the run is
	 * on that axis (a ring lap 410 us, a unit 307 us), the CPU has twice the cycles for each
	 * sample (the per-unit cap doubles) and staging holds twice as long.
	 */
	bool half_rate;
};

/** What a unit's CPU cycles go to (esp_sdr_gapless_stats.cycles). */
enum esp_sdr_gapless_activity {
	/** Kept samples from the ring's blocks to the staging ring. */
	ESP_SDR_GAPLESS_COPY,
	/** From the staging ring into the sink, as int8. */
	ESP_SDR_GAPLESS_PACK,
	/** The sink's service(). */
	ESP_SDR_GAPLESS_SERVICE,
	/** Estimator readings and the keep decisions. */
	ESP_SDR_GAPLESS_WINDOWS,
	/** Markers into the next block. */
	ESP_SDR_GAPLESS_PREP,
	/** The switch, finding the unit, the servos. */
	ESP_SDR_GAPLESS_SWITCH,
	/** Width sorting of the staged samples (esp_sdr_gapless_cfg.wide). */
	ESP_SDR_GAPLESS_SORT,
	ESP_SDR_GAPLESS_ACTIVITIES,
};

/** A run's counters. */
struct esp_sdr_gapless_stats {
	/** Samples the ring delivered, kept and written to the sink, and kept but dropped. */
	uint64_t pairs, kept_pairs, dropped_pairs;
	uint32_t units, windows, loud_windows, kept_windows;
	/** Kept samples lost because their block was needed again before they were copied. */
	uint32_t abandoned_pairs;
	/** How the run ended (enum esp_sdr_gapless_error) and a detail of it. */
	uint32_t error, detail;
	/** Latest switch past the threshold, samples. */
	uint32_t late_max;
	/** Most cycles one pack slice / one service() call took. */
	uint32_t pack_cycles_max, service_cycles_max;
	/** Quiet baseline at the end, window power per sample in raw units squared. */
	uint32_t baseline;
	/** Free cycles left per unit, the lowest seen. */
	uint32_t idle_cycles_min;
	/** Threshold over the baseline at the end, in sixteenths (the servo's). */
	uint32_t ratio_x16;
	/** Receive gain at the end, its steps during the run, the slowest step's cycles. */
	uint32_t gain, gain_steps, gain_cycles_max;
	/** Windows whose estimated peak passed full scale less the low headroom. */
	uint32_t hot_windows;
	/** Samples a unit may keep, at the end (the cap follows what the CPU copies in time). */
	uint32_t cap;
	/** Staging ring: most words in use. */
	uint32_t stage_peak;
	/** Poll loop turns. */
	uint64_t polls;
	/** CPU cycles by enum esp_sdr_gapless_activity, over all units. */
	uint64_t cycles[ESP_SDR_GAPLESS_ACTIVITIES];
	/** The same for the units that ended with the staging ring over half full, and their count. */
	uint64_t busy_cycles[ESP_SDR_GAPLESS_ACTIVITIES];
	uint32_t busy_units;
	/**
	 * The first step that left the switch half the late limit past the threshold: what it
	 * was (enum esp_sdr_gapless_activity + 1, 0: none), its cycles and the cycles it had
	 * before the deadline. A step that overran its budget, for tuning.
	 */
	uint32_t overrun_activity, overrun_cycles, overrun_room;
	/** Width sorting: chunks sent as width records, and the samples they stood for. */
	uint32_t wide_chunks;
	uint64_t wide_pairs;
	/**
	 * Width sorting: chunks judged, and the loud ones by their largest correlation (bin k:
	 * up to (k + 1) / 8 of the power above the noise; wide below 1/2, bins 0 to 3).
	 */
	uint32_t sort_chunks;
	uint32_t sort_hist[8];
	/**
	 * Bursts (runs of kept windows) begun; of them, refused at their start (too little room
	 * in staging) and cut short later (staging full or the unit's cap): their rest went, up to
	 * what a window's staging can reach (what goes on past that counts as a new burst).
	 */
	uint32_t bursts, bursts_refused, bursts_cut;
	/**
	 * Staging words in service at the end (block 3's ring, plus the dump blocks' upper halves
	 * once their canaries came back intact), and canary checks that failed.
	 */
	uint32_t stage_words, spare_errors;
};

/**
 * @brief Run the gapless receiver until @p sink's stop() says so or the ring fails.
 *
 * Needs esp_sdr_init(). Takes the radio lock and holds interrupts off throughout.
 *
 * @param cfg What to keep.
 * @param sink Where kept samples go.
 * @param stats Filled with the run's counters; may be NULL.
 * @retval 0 when stop() ended the run.
 * @retval -EIO when the ring failed (stats->error says why).
 * @retval -EINVAL for a bad argument; -EAGAIN without esp_sdr_init().
 */
int esp_sdr_gapless_run(const struct esp_sdr_gapless_cfg *cfg,
			const struct esp_sdr_gapless_sink *sink, struct esp_sdr_gapless_stats *stats);

/** @brief Forget the learnt baseline (after a retune or a gain change). */
void esp_sdr_gapless_reset(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_GAPLESS_H_ */
