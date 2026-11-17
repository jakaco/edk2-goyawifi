/*
 * goyawifi FD header (first 0x1000 bytes of GOYAWIFI_EFI.fd).
 *
 * - Looks like an ARM zImage (magic 0x016f2818 at +0x24), so S-Boot,
 *   U-Boot bootz/bootm and fastboot accept it.
 * - The EDK2 SEC is linked to run at FD_BASE. If we were entered anywhere
 *   else (U-Boot runs uncompressed Android "kernels" in place, e.g. from
 *   the fastboot buffer), copy the whole FD to FD_BASE first.
 * - Then jump to the FV at FD_BASE + 0x1000, whose zero vector GenFv has
 *   patched with a branch to the SEC entry point.
 *
 * Entered with MMU and D-cache off (Linux boot protocol). r0-r2 preserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef FD_BASE
#define FD_BASE  0x10008000
#endif
#ifndef FD_SIZE
#define FD_SIZE  0x00200000
#endif

	.arm
	.text
	.globl	_start
_start:
	b	reloc			/* 0x00 */
	.rept	7
	mov	r0, r0			/* 0x04 - 0x1c */
	.endr
	b	reloc			/* 0x20 */
	.word	0x016f2818		/* 0x24: zImage magic */
	.word	0			/* 0x28: start (position independent) */
	.word	FD_SIZE			/* 0x2c: end / size */
	.word	0x04030201		/* 0x30: little endian */
	.word	0, 0, 0			/* 0x34 - 0x3f */

reloc:					/* 0x40 */
	adr	r4, _start		/* where we actually are */
	ldr	r5, =FD_BASE
	cmp	r4, r5
	beq	enter

	mov	r6, #FD_SIZE
copy:
	ldmia	r4!, {r7 - r10}
	stmia	r5!, {r7 - r10}
	subs	r6, r6, #16
	bne	copy

	mov	r7, #0
	mcr	p15, 0, r7, c7, c5, 0	/* ICIALLU: drop stale instructions */
	mcr	p15, 0, r7, c7, c5, 6	/* BPIALL */
	dsb
	isb

enter:
	ldr	pc, =(FD_BASE + 0x1000)

	.ltorg
