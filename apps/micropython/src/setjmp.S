/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * setjmp() for MicroPython's exceptions and its garbage collector (linked in with
 * -Wl,--wrap=setjmp, see CMakeLists.txt).
 *
 * The S3's setjmp is the ROM's, which puts the live register windows on the stack with a
 * SYSCALL instruction. Without userspace Zephyr answers that with a register dump on the
 * console for every call, and MicroPython calls setjmp on every try and every garbage
 * collection. This is the same routine (newlib's Xtensa setjmp, Tensilica, MIT license;
 * the jmp_buf layout longjmp in components/esp_rom/patches/esp_rom_longjmp.S expects)
 * with the spill done as Zephyr's own context switch does it (SPILL_ALL_WINDOWS in
 * arch/xtensa/include/xtensa_asm2.inc.S, XCHAL_NUM_AREGS == 64): interrupts masked
 * while the rotated windows have no valid stack pointer.
 */

#include <xtensa/config/core-isa.h>

#if XCHAL_NUM_AREGS != 64
#error setjmp.S is written for 64 address registers
#endif

	.text

/* void esdr_window_spill(void): every window but the caller's... and the caller's frame
 * is then the current one's predecessor, so it is saved too. */
	.align	4
	.type	esdr_window_spill, @function
esdr_window_spill:
	entry	a1, 16
	rsil	a2, XCHAL_EXCM_LEVEL
	and	a12, a12, a12
	rotw	3
	and	a12, a12, a12
	rotw	3
	and	a12, a12, a12
	rotw	3
	and	a12, a12, a12
	rotw	3
	and	a12, a12, a12
	rotw	4
	wsr.ps	a2
	rsync
	retw
	.size	esdr_window_spill, . - esdr_window_spill

/* int setjmp(jmp_buf env) */
	.align	4
	.global	__wrap_setjmp
	.type	__wrap_setjmp, @function
__wrap_setjmp:
	entry	a1, 16

	/* Spill all but the current window. */
	call4	esdr_window_spill

	/* Copy the register save area at (sp - 16) to the jmp_buf. */
	addi	a5, a1, -16
	l32i	a3, a5, 0
	l32i	a4, a5, 4
	s32i	a3, a2, 0
	s32i	a4, a2, 4
	l32i	a3, a5, 8
	l32i	a4, a5, 12
	s32i	a3, a2, 8
	s32i	a4, a2, 12

	/* Copy 0-8 words from the register overflow area. */
	extui	a3, a0, 30, 2
	blti	a3, 2, 1f
	l32i	a7, a1, 4
	slli	a4, a3, 4
	sub	a5, a7, a4
	addi	a6, a2, 16
	addi	a7, a7, -16
0:
	l32i	a3, a5, 0
	l32i	a4, a5, 4
	s32i	a3, a6, 0
	s32i	a4, a6, 4
	l32i	a3, a5, 8
	l32i	a4, a5, 12
	s32i	a3, a6, 8
	s32i	a4, a6, 12
	addi	a5, a5, 16
	addi	a6, a6, 16
	blt	a5, a7, 0b
1:
	/* The 4 words at the stack pointer, in case alloca moves them. */
	l32i	a3, a1, 0
	l32i	a4, a1, 4
	s32i	a3, a2, 48
	s32i	a4, a2, 52
	l32i	a3, a1, 8
	l32i	a4, a1, 12
	s32i	a3, a2, 56
	s32i	a4, a2, 60

	/* The return address, with the window size bits. */
	s32i	a0, a2, 64
	movi	a2, 0
	retw
	.size	__wrap_setjmp, . - __wrap_setjmp
