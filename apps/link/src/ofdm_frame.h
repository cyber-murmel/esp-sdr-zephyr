/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Link frames on the OFDM PHY: a 64 bit header (type, modulation, error
 * correction, addresses, sequence number, CRC-16) on the BPSK header symbols,
 * and a payload carrying the user data and its CRC-32 under one of the codes
 * below (ofdm_fhdr.fec).
 */

#ifndef ESDR_LINK_OFDM_FRAME_H
#define ESDR_LINK_OFDM_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "bch.h"
#include "ofdm.h"

#define OFDM_FRAME_HDR_BITS 64U
/* Payload bytes per frame at most (four bits per sample over a 16 k sample bank). */
#define OFDM_FRAME_CAP_MAX 8192U
#define OFDM_FRAME_UNITS_MAX ((OFDM_FRAME_CAP_MAX + 254U) / 255U)

/*
 * Error correction of the payload, by id (4 bits in the header):
 * - OFDM_FEC_RS: shortened RS(255,223) units byte interleaved over the frame
 *   (a weak subcarrier spreads its errors over all units), 16 byte errors per
 *   unit: for raw bit error rates up to a few 1e-3.
 * - OFDM_FEC_NONE: the CRC-32 alone.
 * - binary BCH codes (bch.h) on contiguous blocks of up to 64, 128 or 256
 *   bytes, 1 to 4 bit errors each ("bch<t>-<bytes>"), and "bch1-8", a Hamming
 *   code of 56 data bits per 64: for rare errors. The CRC-32 is checked first;
 *   only a frame that fails it is decoded block by block.
 */
#define OFDM_FEC_RS   0U
#define OFDM_FEC_NONE 1U
#define OFDM_FECS     15U

/* The code's name, or NULL past the last. */
const char *ofdm_fec_name(unsigned int fec);

/* The code named @p name, or -1. */
int ofdm_fec_parse(const char *name);

struct ofdm_fhdr {
	uint8_t type;
	enum ofdm_mod mod;
	/* OFDM_FEC_RS (0, the default) .. OFDM_FECS - 1. */
	uint8_t fec;
	uint8_t dst, src;
	uint16_t seq;
};

/*
 * Work memory for one build or decode (may be in PSRAM: bch_key marks the code
 * set up in bch, whatever the memory held at boot).
 */
struct ofdm_fwork {
	uint8_t bytes[OFDM_FRAME_CAP_MAX + 1U] __attribute__((aligned(4)));
	uint8_t units[OFDM_FRAME_UNITS_MAX][255];
	uint32_t bch_key;
	struct bch_code bch;
};

/* User data bytes per frame at @p mod under @p fec, 0 if the frame is too short to code. */
size_t ofdm_frame_user_bytes(const struct ofdm_ctx *ctx, enum ofdm_mod mod, unsigned int fec);

/*
 * Build a frame of ofdm_frame_user_bytes(h->mod, h->fec) bytes from @p data into
 * @p out (ctx->len words). Only reads @p ctx.
 *
 * @return ctx->len, 0 for a bad modulation or a frame too short to code.
 */
size_t ofdm_frame_build(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, struct ofdm_fwork *wk,
			const struct ofdm_fhdr *h, const uint8_t *data, uint32_t *out);

/*
 * A header-only frame (an ACK; ofdm_hdr_samples() long) from @p h, and its
 * reception (a window of at least 2 ofdm_hdr_rsamples() + rcp words).
 */
size_t ofdm_frame_build_hdr(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb,
			    const struct ofdm_fhdr *h, uint32_t *out);
int ofdm_frame_begin_hdr(struct ofdm_ctx *ctx, const uint32_t *words, size_t n,
			 struct ofdm_fhdr *h, struct ofdm_rx_info *info);

/*
 * Find a frame in @p n receive words and read its header.
 *
 * @retval 0 on success; the payload follows with ofdm_frame_finish().
 * @retval -1 if no preamble was found.
 * @retval -2 for a header with a bad CRC.
 */
int ofdm_frame_begin(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, struct ofdm_fhdr *h,
		     struct ofdm_rx_info *info);

/*
 * Decode the payload of the frame ofdm_frame_begin() found: @p data gets
 * ofdm_frame_user_bytes(h->mod, h->fec) bytes, @p corrected the byte (RS) or
 * bit (BCH) errors fixed.
 *
 * @retval 0 on success.
 * @retval -3 if a unit or block failed or the CRC-32 does not match.
 */
int ofdm_frame_finish(struct ofdm_ctx *ctx, const uint32_t *words, const struct ofdm_fhdr *h,
		      struct ofdm_fwork *wk, uint8_t *data, int *corrected,
		      struct ofdm_rx_info *info);

/*
 * The FEC half of ofdm_frame_finish(), for a caller that demodulated the
 * payload itself (e.g. on two CPUs with ofdm_rx_part()) into wk->bytes.
 */
int ofdm_frame_fec(const struct ofdm_ctx *ctx, const struct ofdm_fhdr *h, struct ofdm_fwork *wk,
		   uint8_t *data, int *corrected);

/*
 * The same in parts for OFDM_FEC_RS, e.g. on two CPUs: RS units u0 .. u1 - 1
 * (of ofdm_frame_units()) de-interleaved from wk->bytes and decoded, then the
 * user data and its CRC once all units are done. (The other codes decode in
 * one go with ofdm_frame_fec(): a clean frame costs the CRC-32.)
 */
unsigned int ofdm_frame_units(const struct ofdm_ctx *ctx, enum ofdm_mod mod);
int ofdm_frame_fec_units(const struct ofdm_ctx *ctx, const struct ofdm_fhdr *h,
			 struct ofdm_fwork *wk, unsigned int u0, unsigned int u1, int *corrected);
int ofdm_frame_check(const struct ofdm_ctx *ctx, const struct ofdm_fhdr *h, struct ofdm_fwork *wk,
		     uint8_t *data);

#endif
