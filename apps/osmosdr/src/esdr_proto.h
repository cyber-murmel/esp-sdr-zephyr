/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB protocol of the osmosdr app, shared with the host tools: vendor
 * control requests to the device (recipient device) set the radio, bulk
 * endpoints carry the samples, after HackRF's model.
 *
 * - Bulk IN (0x81): receive blocks of ESDR_BLOCK_BYTES, a header then
 *   interleaved I/Q (int8, or int16 little endian). The device sends whole
 *   blocks (multiples of 64 bytes, no short packets), so any read size that
 *   is a multiple of ESDR_BLOCK_BYTES stays aligned to them.
 * - Bulk OUT: transmit samples, interleaved I/Q in the format set (int8, or
 *   int16 little endian), at the sample rate set. The device takes them as
 *   fast as the DAC plays them: USB flow control paces the host, about 50 ms
 *   ahead. Input that runs out plays as zeros.
 *
 * All multi-byte fields are little endian. Plain C99, no Zephyr headers.
 */

#ifndef ESDR_PROTO_H_
#define ESDR_PROTO_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ESDR_PROTO_VERSION 1U

/* Vendor requests (bRequest). */
enum esdr_req {
	/* IN: struct esdr_info. */
	ESDR_REQ_GET_INFO = 0x01,
	/* wValue: enum esdr_mode, ESDR_MODE_OFF optionally with ESDR_MODE_F_DRAIN. */
	ESDR_REQ_SET_MODE = 0x02,
	/* OUT 8 bytes: centre frequency in Hz (uint64_t). */
	ESDR_REQ_SET_FREQ = 0x03,
	/* OUT 4 bytes: sample rate in Hz (uint32_t); the nearest supported one is taken. */
	ESDR_REQ_SET_SAMPLE_RATE = 0x04,
	/* wValue: PHY gain index (0 to info.rx_gain_max), or ESDR_GAIN_AUTO. */
	ESDR_REQ_SET_RX_GAIN = 0x05,
	/* wValue: transmit power step (0 weakest to info.tx_gain_max). */
	ESDR_REQ_SET_TX_GAIN = 0x06,
	/* OUT 4 bytes: analog low-pass bandwidth in Hz (uint32_t), 0 for automatic. */
	ESDR_REQ_SET_BANDWIDTH = 0x07,
	/*
	 * wValue: bits per I and Q item, 8 or 16, or 4 where the device offers it
	 * (the S3 osmosdr app: one byte per sample, I in bits 3:0 and Q in bits
	 * 7:4, each signed, full scale 8; receive only, transmit items stay 8 bit).
	 */
	ESDR_REQ_SET_FORMAT = 0x08,
	/* OUT 4 bytes: crystal correction in parts per billion (int32_t). */
	ESDR_REQ_SET_FREQ_CORR = 0x09,
	/* IN: struct esdr_state. */
	ESDR_REQ_GET_STATE = 0x0a,
	/* IN: struct esdr_stats. */
	ESDR_REQ_GET_STATS = 0x0b,
	/* wValue: ESDR_OPT_* bits. */
	ESDR_REQ_SET_OPTIONS = 0x0c,
	/*
	 * wValue: digital gain in dB before the items are rounded, a multiple of
	 * 6 up to ESDR_DGAIN_MAX_DB, or ESDR_DGAIN_DEFAULT (24 dB at 8 bit, 0 at
	 * 16). Decimation leaves a quiet band's noise far below an 8-bit item.
	 */
	ESDR_REQ_SET_DIGITAL_GAIN = 0x0d,
	/*
	 * Serial transport only. wValue: native rate in MHz (80 on the ESP32-C6);
	 * OUT 4 bytes: complex samples (uint32_t, up to ESDR_BURST_MAX). One
	 * burst at the centre frequency, contiguous, as int16 receive blocks
	 * flagged ESDR_BLK_BURST before the reply (full scale 16384, the raw
	 * 10-bit samples times 32). A running stream stops.
	 */
	ESDR_REQ_CAPTURE_BURST = 0x0e,
	/*
	 * Serial transport, ESP32-C6. wValue: trigger threshold over the quiet
	 * baseline in sixteenths (0: 128, 9 dB); OUT 0 or 4 bytes: samples kept
	 * after the trigger (uint32_t, below ESDR_TRIG_SAMPLES; default half).
	 * Applies to ESDR_MODE_TRIGGER, now or next.
	 */
	ESDR_REQ_SET_TRIGGER = 0x0f,
	/*
	 * Back to the settings the device started with (mode off, frequency, gains,
	 * format, rate, filter, options, trigger): what a host leaves behind for
	 * the next one. A running stream stops.
	 */
	ESDR_REQ_RESET = 0x10,
	/*
	 * Serial transport, ESP32-C6 with CONFIG_ESP_SDR_GAPLESS. OUT struct
	 * esdr_gapless_cfg: which windows of the gapless 80 MS/s stream are kept.
	 * Applies to ESDR_MODE_GAPLESS, now or next.
	 */
	ESDR_REQ_SET_GAPLESS = 0x11,
	/* Serial transport, ESP32-C6. IN: struct esdr_gapless_stats of the current or last run. */
	ESDR_REQ_GET_GAPLESS_STATS = 0x12,
	/*
	 * P4 bridge (apps/osmosdr_bridge) over a gapless C6. OUT struct esdr_chan_cfg: the
	 * channelizer the bridge runs on the segments; channels 0 forwards the segments as
	 * they come (ESDR_BLK_SEG). IN: struct esdr_chan_stats.
	 */
	ESDR_REQ_SET_CHANNELIZER = 0x13,
	/*
	 * WHY2025 badge bridge (apps/osmosdr_bridge), answered by the P4 itself. wValue: seconds
	 * (0: 30). After the reply the P4 hands USB-C port J8 to the ESP32-C6 (its
	 * USB-Serial-JTAG, for esptool) for that long, then takes it back.
	 */
	ESDR_REQ_USB_TO_C6 = 0x14,
	/*
	 * Serial transport, a device the ESP-IDF bootloader starts (the
	 * ESP32-P4-Function-EV-Board's ESP32-C6): firmware update. OUT 4 bytes:
	 * image size (uint32_t). The image goes to the OTA partition the device
	 * does not run from.
	 */
	ESDR_REQ_FW_BEGIN = 0x15,
	/*
	 * OUT: offset (uint32_t), then the image bytes from there (a multiple of
	 * 4, up to ESDR_FW_CHUNK), in order.
	 */
	ESDR_REQ_FW_WRITE = 0x16,
	/*
	 * OUT 4 bytes: CRC-32 (IEEE, as zlib's) of the whole image. The device
	 * checks it, selects the image and restarts after the reply.
	 */
	ESDR_REQ_FW_END = 0x17,
};

#define ESDR_BURST_MAX 16380U

/* Image bytes per ESDR_REQ_FW_WRITE: a frame's data less the offset. */
#define ESDR_FW_CHUNK 508U

enum esdr_mode {
	ESDR_MODE_OFF = 0,
	ESDR_MODE_RX = 1,
	ESDR_MODE_TX = 2,
	/*
	 * Serial transport, ESP32-C6: triggered snapshots. The receiver watches
	 * the power of the whole 80 MHz input; a burst past the threshold
	 * (ESDR_REQ_SET_TRIGGER) sends ESDR_TRIG_SAMPLES samples at 80 MS/s from
	 * before and after it as ESDR_TRIG_BLOCKS int8 blocks flagged
	 * ESDR_BLK_TRIGGER (the first also ESDR_BLK_GAP), whatever the format.
	 * Some 4 ms pass before the next can trigger.
	 */
	ESDR_MODE_TRIGGER = 3,
	/*
	 * Serial transport, ESP32-C6 with CONFIG_ESP_SDR_GAPLESS: the whole 80 MHz
	 * input without a gap, pruned to what the link carries. The receiver keeps the
	 * 10 us windows whose power passes the threshold (ESDR_REQ_SET_GAPLESS), with
	 * windows before and after them, and sends them as ESDR_BLK_SEG blocks. Every
	 * sample keeps its index on the 80 MS/s axis, so the host sees what was left out.
	 */
	ESDR_MODE_GAPLESS = 4,
	/* Throughput tests: IN sends counting blocks as fast as the bus takes them, OUT sinks. */
	ESDR_MODE_TEST_IN = 0x10,
	ESDR_MODE_TEST_OUT = 0x11,
};

/*
 * Serial transport only: with ESDR_MODE_OFF, transmit mode ends once the samples queued for it
 * have played, and the reply comes then (a burst is over when its request returns).
 */
#define ESDR_MODE_F_DRAIN 0x8000U

#define ESDR_GAIN_AUTO 0xffffU
#define ESDR_DGAIN_DEFAULT 0xffffU
#define ESDR_DGAIN_MAX_DB  42U

/*
 * Offset tuning (default on): the LO sits 4 MHz below the centre and the
 * stream is mixed down digitally, so the LO leakage and the 1/f noise at
 * the LO stay outside the output band.
 */
#define ESDR_OPT_OFFSET_TUNING 0x0001U

#define ESDR_INFO_MAGIC 0x52445345U /* "ESDR" */

struct esdr_info {
	uint32_t magic;
	uint16_t proto_version;
	uint16_t info_size;
	char firmware[32];
	uint64_t freq_min_hz, freq_max_hz;
	/* Supported receive rates: rx_rate_max_hz >> k for k = 0 .. rx_rate_count - 1. */
	uint32_t rx_rate_max_hz;
	uint16_t rx_rate_count;
	uint16_t rx_gain_max;
	uint16_t tx_gain_max;
	uint16_t block_bytes;
	/* Full scale of the 16-bit items (8-bit items: 128). */
	uint16_t full_scale16;
	uint16_t reserved;
} __attribute__((packed));

struct esdr_state {
	uint16_t mode;
	uint16_t bits;
	uint16_t options;
	/* Gain index or ESDR_GAIN_AUTO; transmit step. */
	uint16_t rx_gain, tx_gain;
	/* Digital gain in dB as applied. */
	uint16_t digital_gain_db;
	/* Requested centre, and where the stream is actually centred (Hz, rounded). */
	uint64_t freq_hz;
	uint64_t center_hz;
	uint32_t sample_rate_hz;
	uint32_t bandwidth_hz;
	int32_t freq_corr_ppb;
	/* LO as programmed (MHz plus kHz offset). */
	uint32_t lo_mhz;
	int32_t lo_offset_khz;
} __attribute__((packed));

struct esdr_stats {
	/* Samples delivered to USB, and samples dropped for want of free blocks. */
	uint64_t rx_samples, rx_overflow_samples;
	/* Input lost in the ring (filter too slow), and samples lost to restarts. */
	uint64_t rx_ring_lost_pairs, rx_restart_samples;
	uint64_t usb_in_bytes, usb_out_bytes;
	uint32_t rx_blocks, rx_overflows;
	uint32_t ring_runs, ring_errors, ring_late_max, ring_cycles_x100;
	uint32_t ring_abandoned, ring_gain_refreshed;
	/* Last ring error (enum esp_sdr_ring_status) and its detail. */
	uint32_t ring_last_status, ring_last_detail;
	/* Transmit: samples played, and dropped (late, or lost in a DAC restart). */
	uint64_t tx_samples, tx_dropped;
	/* Blocks the DAC played short of input (zero filled), sessions, DAC restarts. */
	uint32_t tx_underruns, tx_sessions, tx_restarts;
	/* Largest backlog of samples waiting for the DAC. */
	uint32_t tx_queued_max;
} __attribute__((packed));

#define ESDR_BLOCK_BYTES 4096U
#define ESDR_BLOCK_MAGIC 0x4b4c4245U /* "EBLK" */

/* Block flags. */
/* The block's index does not follow the previous block's: samples are missing before it. */
#define ESDR_BLK_GAP      0x0001U
/* Blocks were dropped before this one (the host did not read in time). */
#define ESDR_BLK_OVERFLOW 0x0002U
/* Items are int16 (else int8). */
#define ESDR_BLK_16BIT    0x0004U
/* Counting test pattern, not samples. */
#define ESDR_BLK_TEST     0x0008U
/* A burst's block (ESDR_REQ_CAPTURE_BURST): its index counts from the burst's first sample. */
#define ESDR_BLK_BURST    0x0010U
/* Items are 4 bit, one byte per sample (ESDR_REQ_SET_FORMAT 4). */
#define ESDR_BLK_4BIT     0x0020U
/*
 * A triggered snapshot's block (ESDR_MODE_TRIGGER), 80 MS/s: its index is the
 * first sample's position relative to the trigger, an int64_t (negative
 * before it).
 */
#define ESDR_BLK_TRIGGER  0x0040U

/*
 * A gapless block (ESDR_MODE_GAPLESS), int8: segments of the 80 MS/s stream.
 * The header's index is the first segment's, samples the pairs of all its
 * segments. The payload holds segments back to back, each a struct
 * esdr_seg_hdr then its pairs (I then Q, int8), padded to 4 bytes; a segment
 * header with no pairs, or the payload's end, ends the block.
 */
#define ESDR_BLK_SEG      0x0080U
/*
 * A channel block (ESDR_REQ_SET_CHANNELIZER), int16: runs of the bridge's channelizer output,
 * each one channel's consecutive samples. The payload holds runs back to back, each a struct
 * esdr_chan_hdr then its samples (I then Q, int16 little endian); a run header with no samples,
 * or the payload's end, ends the block. The block header's index is the first run's (counted
 * at the channel rate, 80 MS/s over esdr_chan_hdr.decim, from the stream's start), its samples
 * those of all runs.
 */
#define ESDR_BLK_CHAN     0x0100U

struct esdr_block_hdr {
	uint32_t magic;
	uint16_t flags;
	/* Complex samples in this block. */
	uint16_t samples;
	/* Index of the first sample on the stream's time axis (rate since the stream started). */
	uint64_t index;
} __attribute__((packed));

#define ESDR_BLOCK_PAYLOAD (ESDR_BLOCK_BYTES - sizeof(struct esdr_block_hdr))

/* All block flags above. */
#define ESDR_BLK_KNOWN                                                                             \
	(ESDR_BLK_GAP | ESDR_BLK_OVERFLOW | ESDR_BLK_16BIT | ESDR_BLK_TEST | ESDR_BLK_BURST |      \
	 ESDR_BLK_4BIT | ESDR_BLK_TRIGGER | ESDR_BLK_SEG | ESDR_BLK_CHAN)

/*
 * A plausible block header at p (the magic alone could occur in sample data): no flag outside
 * accept, and no more samples than the payload holds at the item width. For a host that
 * resynchronises on the receive stream.
 */
static inline bool esdr_block_hdr_ok(const void *p, uint16_t accept)
{
	struct esdr_block_hdr h;

	memcpy(&h, p, sizeof(h));
	return h.magic == ESDR_BLOCK_MAGIC && (h.flags & ~(uint32_t)accept) == 0U &&
	       h.samples <= ESDR_BLOCK_PAYLOAD / ((h.flags & ESDR_BLK_4BIT) != 0U ? 1U : 2U);
}

/* Segment flags. */
/* Windows that passed the threshold were dropped before this segment: the link was full. */
#define ESDR_SEG_DROPPED 0x01U
/* The ring restarted before this segment (an error): the indices before it are an estimate. */
#define ESDR_SEG_RESTART 0x02U
/* The segment's first window passed the threshold (else it is a window before or after one). */
#define ESDR_SEG_LOUD    0x04U
/* Gain index of a segment taken with the receiver's automatic gain. */
#define ESDR_SEG_GAIN_AUTO 0xffU

struct esdr_seg_hdr {
	/* The first pair's index less the block header's index. */
	uint32_t delta;
	uint16_t pairs;
	/* Right shift from the 10-bit raw samples to int8 (saturated), 0 to 2. */
	uint8_t shift;
	uint8_t flags;
	/* Power of the segment's first window in dB below full scale, times 10 (negative). */
	int16_t power_dbfs_x10;
	/*
	 * The receiver's DC when the segment was kept, raw units: it is left in the items, so a
	 * sample is (item << shift) - dc.
	 */
	int8_t dc_i, dc_q;
	/* Receive gain index the samples were taken at (255: automatic gain). */
	uint8_t gain;
	uint8_t reserved[3];
} __attribute__((packed));

/* Pairs in a gapless block whose payload is one segment. */
#define ESDR_SEG_PAIRS_MAX ((ESDR_BLOCK_PAYLOAD - sizeof(struct esdr_seg_hdr)) / 2U)
#define ESDR_GAPLESS_RATE_HZ 80000000U
/* One window of the IQ estimator, the unit of the keep decision: 10 us. */
#define ESDR_GAPLESS_WINDOW_PAIRS 800U

/* Gapless options. */
/* Step the receive gain to keep the peaks in the headroom band (default with ESDR_GAIN_AUTO). */
#define ESDR_GAPLESS_F_GAIN_SERVO 0x0001U
/* Keep the threshold where ratio_x16 puts it: no servo against the link's budget. */
#define ESDR_GAPLESS_F_FIXED_THR  0x0002U

struct esdr_gapless_cfg {
	/* Threshold over the quiet baseline in sixteenths, the servo's floor (0: 64, 6 dB). */
	uint16_t ratio_x16;
	/* Windows kept before and after a window past the threshold (0xff: default 1, at most 4). */
	uint8_t pre, post;
	/* Fixed threshold, window power per pair in raw units squared (0: from the baseline). */
	uint32_t level;
	/* Payload bytes per second the device may send (0: what the link carries). */
	uint32_t budget_bps;
	/* ESDR_GAPLESS_F_*. */
	uint16_t flags;
	/* Headroom band of both servos in percent (0, 0: 5 and 10). */
	uint8_t headroom_lo, headroom_hi;
} __attribute__((packed));

struct esdr_gapless_stats {
	/*
	 * Pairs the ring delivered; kept and sent; kept but dropped (no room in the staging ring
	 * or past the unit's cap); kept but lost (their block was needed again first).
	 */
	uint64_t pairs, kept_pairs, dropped_pairs, abandoned_pairs;
	/* Runs (a request or a ring error ends one), units, windows, past the threshold, kept. */
	uint32_t runs, units, windows, loud_windows, kept_windows;
	/* Ring errors (each restarts the run), the last one (esp_sdr_gapless.h) and its detail. */
	uint32_t ring_errors, ring_last_error, ring_last_detail;
	/* Latest switch past the threshold, pairs; fewest free CPU cycles a unit had. */
	uint32_t late_max, idle_cycles_min;
	/* Staging ring: size and highest fill, bytes. */
	uint32_t stage_bytes, stage_peak_bytes;
	/* Quiet baseline (window power per pair, raw units squared), threshold over it (/16). */
	uint32_t baseline, ratio_x16;
	/* Receive gain index now, its steps. */
	uint32_t gain, gain_steps;
	/* Blocks sent; segments the link had no room for at once (they waited). */
	uint32_t blocks_sent, link_full;
	/*
	 * The last run's first step that made a switch late (esp_sdr_gapless.h: its activity + 1,
	 * 0 for none), its CPU cycles and the cycles it had before the deadline.
	 */
	uint32_t overrun_activity, overrun_cycles, overrun_room;
} __attribute__((packed));

/* Channel block flags. */
/* The first samples after a gap: the filter history was zeros. */
#define ESDR_CHAN_TRANSIENT 0x01U
/* The channel's burst detector opened at this block / closed after it. */
#define ESDR_CHAN_OPEN      0x02U
#define ESDR_CHAN_CLOSE     0x04U

struct esdr_chan_hdr {
	/* Channel 0 is the centre; channel k sits k * 80 MHz / channels above it (k > channels / 2: below). */
	uint16_t channel, channels;
	/* Channel rate is ESDR_GAPLESS_RATE_HZ / decim. */
	uint16_t decim;
	uint8_t flags;
	uint8_t reserved;
	/* The run's first sample index less the block header's index; its samples. */
	uint32_t delta;
	uint16_t samples;
	uint16_t reserved2;
} __attribute__((packed));

struct esdr_chan_stats {
	/* Segment blocks taken, and dropped (the channelizer fell behind). */
	uint32_t seg_blocks, seg_dropped;
	/* Channel blocks sent, and dropped (no free buffer, or USB refused). */
	uint32_t chan_blocks, chan_dropped;
	/* Input pairs channelized, and the CPU cycles that took (filterbank, detector, packing). */
	uint64_t pairs, cycles;
	uint32_t cpu_mhz;
	/* Of the cycles: the detector's, and the channel blocks' (taking, packing, sending). */
	uint64_t det_cycles, send_cycles;
	/* The filterbank's branch sums and FFTs, when built with ESDR_PFBQ_PROFILE (else 0). */
	uint64_t branch_cycles, fft_cycles;
} __attribute__((packed));

struct esdr_chan_cfg {
	/* Channels over the 80 MHz (16, 32, 40, 64 or 80), 0 for no channelizer. */
	uint16_t channels;
	/* Burst detector: open above the channel's noise floor by this many dB, times 10. */
	uint16_t open_db_x10;
	/* Bitmap of channels to send (bit k = channel k); all ones for every active one. */
	uint32_t mask[3];
} __attribute__((packed));

/* A triggered snapshot: whole int8 blocks. */
#define ESDR_TRIG_BLOCKS  8U
#define ESDR_TRIG_SAMPLES (ESDR_TRIG_BLOCKS * (ESDR_BLOCK_PAYLOAD / 2U))
#define ESDR_TRIG_RATE_HZ 80000000U

/*
 * Serial transport (apps/osmosdr_serial, ESP32-C6 over USB-Serial-JTAG: one
 * byte stream each way). The vendor requests travel as frames: struct
 * esdr_ser_hdr, then len bytes.
 *
 * - Host to device: req is an enum esdr_req. An OUT request carries its data
 *   (len bytes) and wValue; an IN request (ESDR_SER_IN) carries none, len is
 *   the most it accepts. ESDR_SER_TX_DATA frames carry transmit samples.
 * - Device to host: every request gets one reply frame with the same req,
 *   status 0 or a negative errno, and the IN data. Receive blocks (as on USB,
 *   starting with ESDR_BLOCK_MAGIC) come between frames, never inside one.
 *
 * A request other than ESDR_SER_TX_DATA stops a running stream for as long as
 * it takes to apply it; the next receive block then carries ESDR_BLK_GAP.
 */
#define ESDR_SER_MAGIC 0x4c525345U /* "ESRL" */
#define ESDR_SER_IN      0x01U
#define ESDR_SER_TX_DATA 0x80U
#define ESDR_SER_MAX_DATA 512U

struct esdr_ser_hdr {
	uint32_t magic;
	uint8_t req;
	uint8_t flags;
	uint16_t value;
	uint16_t len;
	int16_t status;
} __attribute__((packed));

#endif /* ESDR_PROTO_H_ */
