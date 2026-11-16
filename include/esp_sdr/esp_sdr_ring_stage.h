/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief The receiver's output staged in internal RAM for a consumer on the other CPU.
 *
 * On the S3 a ring run with esp_sdr_ring_cfg::spare_bank_app leaves SRAM bank 2 to the app;
 * with esp_sdr_stage_sink() as the run's sink, each output sample goes there as one word (I in
 * the low half, Q in the high: in memory an int16 I, Q pair), into a ring of
 * ESP_SDR_STAGE_WORDS words; optionally two consecutive outputs are averaged into one (a boxcar
 * of 2: 4 MS/s from ESP_SDR_RING_DECIM_BOX4 as 2 MS/s). On the S31 the stage lies in the capture
 * bank (TCM group 3) past the dump ring's words, and a dump ring run's own decimator writes it
 * (esp_sdr_stage_put()). Either way the writer runs from IRAM and touches internal RAM only,
 * so the other CPU's flash and PSRAM traffic cannot stall it.
 *
 * The consumer reads in place (esp_sdr_stage_peek(), esp_sdr_stage_advance()) on its own
 * stream index: samples are counted from the stage's start, a hole in the ring's output (lost
 * input) moves the index on by the samples lost, a new ring run by one. The sink never waits: a
 * consumer that falls a whole stage behind is told and resumes half a stage back; one whose
 * stage was initialised again (esp_sdr_stage_init()) starts over at its oldest sample.
 * Needs CONFIG_ESP_SDR_RING_STAGE (the ESP32-S3 with the ring, the ESP32-S31 with the dump ring
 * and group 2).
 */

#ifndef ESP_SDR_RING_STAGE_H_
#define ESP_SDR_RING_STAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Samples the stage holds (64 KiB less its header). */
#define ESP_SDR_STAGE_WORDS 16240U

/** A consumer's place in the stage. */
struct esp_sdr_stage_reader {
	/** Samples consumed, counted as the sink counts them written, and where the next is. */
	uint32_t pos, rpos;
	/** Hole log entries applied. */
	uint32_t holes;
	/** The stream index of the sample at pos. */
	uint64_t index;
	/** Times the sink lapped the reader (samples lost), or the stage started over. */
	uint32_t lapped;
	/** The esp_sdr_stage_init() the reader follows. */
	uint32_t gen;
};

/**
 * @brief Clear the stage before a capture; @p halve averages the outputs in pairs.
 * Bank 2 must be the app's (no ring run without spare_bank_app, no burst capture) from here
 * until the capture ends.
 */
void esp_sdr_stage_init(bool halve);

/** @brief Before each esp_sdr_ring_run(): its output index starts at 0 again (a hole). */
void esp_sdr_stage_run_start(void);

/** @brief The sink for esp_sdr_ring_cfg (user unused). */
void esp_sdr_stage_sink(void *user, const int16_t *i, const int16_t *q, size_t n, uint64_t index);

/**
 * @brief Samples into the stage from a writer of its own (the S31's dump ring decimator).
 * @param iq n int16 I, Q pairs, 4-byte aligned.
 * @param n How many.
 * @param index The first one's stream index before any halving (one past the last call's
 *	  last: no hole).
 */
void esp_sdr_stage_put(const int16_t *iq, size_t n, uint64_t index);

/** @brief Samples written since esp_sdr_stage_init(), modulo 2^32. */
uint32_t esp_sdr_stage_written(void);

/** @brief A reader at the oldest sample the stage holds now (before the run: at its start). */
void esp_sdr_stage_reader_init(struct esp_sdr_stage_reader *r);

/**
 * @brief The samples after @p r's position that follow each other: up to @p max words, none
 * past the stage's end or a hole (the next call starts after it, on the new index).
 * @param r The reader.
 * @param w Their first word.
 * @param index The stream index of the first.
 * @param max Words at most.
 * @return How many (0: nothing new). A reader the sink lapped resumes half a stage back
 *         (r->lapped counts it); the index jumps over what was lost.
 */
uint32_t esp_sdr_stage_peek(struct esp_sdr_stage_reader *r, const uint32_t **w, uint64_t *index,
			    uint32_t max);

/**
 * @brief Done with @p n samples of the last peek. Returns false when the sink overwrote them
 * meanwhile (the reader was too slow: what it read may be newer samples).
 */
bool esp_sdr_stage_advance(struct esp_sdr_stage_reader *r, uint32_t n);

#endif /* ESP_SDR_RING_STAGE_H_ */
