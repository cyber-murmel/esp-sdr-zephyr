/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A virtual CC2400 register file: the host tools read and write registers
 * (READ_REGISTER, WRITE_REGISTER(S), READ_ALL_REGISTERS; ubertooth-debug and
 * the range of "-r/-w" style uses) and the upstream firmware code paths that
 * we port program a few of them. There is no CC2400 here, so a write is only
 * stored and, for the registers the radio glue cares about, reported to a
 * hook.
 */

#ifndef UBT_REGS_H_
#define UBT_REGS_H_

#include <stddef.h>
#include <stdint.h>

/* The CC2400 registers the glue reacts to (upstream firmware/common/cc2400.h). */
#define UBT_REG_MAIN    0x00U
#define UBT_REG_FSDIV   0x02U
#define UBT_REG_MDMCTRL 0x03U
#define UBT_REG_FREND   0x05U
#define UBT_REG_RSSI    0x06U
#define UBT_REG_GRMDM   0x20U
#define UBT_REG_SYNCL   0x2cU
#define UBT_REG_SYNCH   0x2dU

/* Registers 0x00 to MAX are readable (READ_ALL_REGISTERS returns 0x2d records of 3 bytes, as upstream). */
#define UBT_REG_MAX 0x2dU

struct ubt_regs {
	uint16_t r[UBT_REG_MAX + 1U];
	/* Called after a write to a stored register, may be NULL. */
	void (*on_write)(void *ctx, uint8_t reg, uint16_t val);
	void *ctx;
};

void ubt_regs_init(struct ubt_regs *g);

/* Unknown registers read as 0 and ignore writes. */
uint16_t ubt_regs_read(const struct ubt_regs *g, uint8_t reg);
void ubt_regs_write(struct ubt_regs *g, uint8_t reg, uint16_t val);

/* READ_ALL_REGISTERS reply: records {reg, hi, lo} for 0..0x2c, returns the byte count (0x2d * 3). */
size_t ubt_regs_dump(const struct ubt_regs *g, uint8_t *buf, size_t max);

#endif /* UBT_REGS_H_ */
