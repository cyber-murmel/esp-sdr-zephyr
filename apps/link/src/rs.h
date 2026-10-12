/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reed-Solomon RS(255,255-n) over GF(2^8): polynomial 0x11d, first
 * consecutive root alpha^1, n parity bytes (32 by default, CONFIG_APP_RS_PARITY),
 * corrects up to n/2 byte errors per codeword. Portable C, no Zephyr dependencies.
 */

#ifndef LINK_RS_H_
#define LINK_RS_H_

#include <stdint.h>

#ifdef CONFIG_APP_RS_PARITY
#define RS_NROOTS CONFIG_APP_RS_PARITY
#else
#define RS_NROOTS 32
#endif
#define RS_N      255
#define RS_K      (RS_N - RS_NROOTS)

/* Build the field and encoder tables; call once before the others. */
void rs_init(void);

/* Append RS_NROOTS parity bytes to cw[0..RS_K) in cw[RS_K..RS_N). */
void rs_encode(uint8_t *cw);

/*
 * Correct cw in place. Returns the number of corrected bytes (0 for a clean
 * codeword) or -1 if it is not correctable.
 */
int rs_decode(uint8_t *cw);

#endif /* LINK_RS_H_ */
