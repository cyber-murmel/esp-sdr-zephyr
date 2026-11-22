/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, expansion kernels: see txs_expand.h.
 */

#include <string.h>

#include <esp_sdr/esp_sdr_place.h>

#include "txs_expand.h"

/* The reference: one sample at a time through memcpy, any alignment. */
static void ref_iq8(uint32_t *dst, const uint8_t *src, uint32_t n)
{
	for (uint32_t k = 0; k < n; k++) {
		uint16_t x;

		memcpy(&x, src + 2U * k, sizeof(x));
		dst[k] = txs_iq8_word(x);
	}
}

static void ref_word32(uint32_t *dst, const uint8_t *src, uint32_t n)
{
	memcpy(dst, src, 4U * (size_t)n);
}

static void ref_zero(uint32_t *dst, uint32_t n)
{
	memset(dst, 0, 4U * (size_t)n);
}

const struct txs_kern txs_kern_ref = {
	.name = "ref", .iq8 = ref_iq8, .word32 = ref_word32, .zero = ref_zero,
};

/* Loads through the packet's bytes: an aligned type that may alias them. */
typedef uint16_t __attribute__((may_alias)) u16a;
typedef uint32_t __attribute__((may_alias)) u32a;

static inline uint32_t ld16(const uint8_t *p)
{
	return *(const u16a *)__builtin_assume_aligned(p, 2);
}

ESP_SDR_CODE(PACED_SAMPLE, txs_fast_iq8) __attribute__((optimize("O3")))
static void txs_fast_iq8(uint32_t *dst, const uint8_t *src, uint32_t n)
{
	const u32a *s;

	if (n != 0U && ((uintptr_t)src & 2U) != 0U) {
		*dst++ = txs_iq8_word(ld16(src));
		src += 2;
		n--;
	}
	s = (const u32a *)__builtin_assume_aligned(src, 4);
	for (; n >= 2U; n -= 2U) {
		uint32_t v = *s++;

		/* sample 0: the low half of v, sample 1: the high half */
		dst[0] = ((v & 0xffU) << 2) | ((v & 0xff00U) << 4);
		dst[1] = ((v >> 14) & 0x3fcU) | ((v >> 12) & 0xff000U);
		dst += 2;
	}
	if (n != 0U) {
		*dst = txs_iq8_word(ld16((const uint8_t *)s));
	}
}

ESP_SDR_CODE(PACED_SAMPLE, txs_fast_word32) __attribute__((optimize("O3")))
static void txs_fast_word32(uint32_t *dst, const uint8_t *src, uint32_t n)
{
	const u32a *s;
	uint32_t h;

	if (((uintptr_t)src & 2U) == 0U) {
		s = (const u32a *)__builtin_assume_aligned(src, 4);
		for (; n >= 4U; n -= 4U) {
			uint32_t a = s[0], b = s[1], c = s[2], d = s[3];

			dst[0] = a;
			dst[1] = b;
			dst[2] = c;
			dst[3] = d;
			s += 4;
			dst += 4;
		}
		while (n-- != 0U) {
			*dst++ = *s++;
		}
		return;
	}
	if (n == 0U) {
		return;
	}
	/* Words straddle two aligned ones: the first halfword, then aligned loads, then the last
	 * halfword (an aligned load of it would read 2 bytes past the payload).
	 */
	h = ld16(src);
	s = (const u32a *)__builtin_assume_aligned(src + 2, 4);
	/* four words a turn, the loads first: the TCM's load to use stall would otherwise cost more than
	 * the shifts (9 cycles a word measured for the plain loop)
	 */
	for (; n > 4U; n -= 4U) {
		const uint32_t a = s[0], b = s[1], c = s[2], d = s[3];

		dst[0] = h | (a << 16);
		dst[1] = (a >> 16) | (b << 16);
		dst[2] = (b >> 16) | (c << 16);
		dst[3] = (c >> 16) | (d << 16);
		h = d >> 16;
		s += 4;
		dst += 4;
	}
	for (; n > 1U; n--) {
		uint32_t w = *s++;

		*dst++ = h | (w << 16);
		h = w >> 16;
	}
	*dst = h | (ld16((const uint8_t *)s) << 16);
}

ESP_SDR_CODE(PACED_SAMPLE, txs_fast_zero) __attribute__((optimize("O3")))
static void txs_fast_zero(uint32_t *dst, uint32_t n)
{
	for (; n >= 4U; n -= 4U) {
		dst[0] = 0;
		dst[1] = 0;
		dst[2] = 0;
		dst[3] = 0;
		dst += 4;
	}
	while (n-- != 0U) {
		*dst++ = 0;
	}
}

const struct txs_kern txs_kern_fast = {
	.name = "fast", .iq8 = txs_fast_iq8, .word32 = txs_fast_word32, .zero = txs_fast_zero,
};

const struct txs_kern *txs_kern_find(const char *name)
{
	if (strcmp(name, "ref") == 0) {
		return &txs_kern_ref;
	}
	if (strcmp(name, "fast") == 0) {
		return &txs_kern_fast;
	}
#ifdef CONFIG_ESP_SDR_TXS_PIE
	if (strcmp(name, "pie") == 0) {
		return &txs_kern_pie;
	}
#endif
	return NULL;
}
