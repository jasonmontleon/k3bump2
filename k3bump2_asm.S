/* entry, system calls, and the two ends of the replayed window.  The window never uses gp, so gp carries
 * the pointer to struct ctx while the borrowed code runs; t6 is free at the window's first instruction
 * (a function-call boundary), so the jump into the window goes through it. */
	.option norelax
	.text
	.globl _start
_start:
	ld	a0, 0(sp)		/* argc */
	addi	a1, sp, 8		/* argv */
	la	sp, mystack_top		/* own stack inside this image: the kernel-provided one may be mapped over */
	call	k3main
1:	li	a7, 94
	li	a0, 3
	ecall
	j	1b

	.globl sys6
sys6:	mv	a7, a0
	mv	a0, a1
	mv	a1, a2
	mv	a2, a3
	mv	a3, a4
	mv	a4, a5
	mv	a5, a6
	ecall
	ret

	.globl enter_window
enter_window:				/* a0 = struct ctx * */
	la	t0, save_area
	sd	ra, 0(t0)
	sd	sp, 8(t0)
	sd	gp, 16(t0)
	sd	tp, 24(t0)
	sd	s0, 32(t0)
	sd	s1, 40(t0)
	sd	s2, 48(t0)
	sd	s3, 56(t0)
	sd	s4, 64(t0)
	sd	s5, 72(t0)
	sd	s6, 80(t0)
	sd	s7, 88(t0)
	sd	s8, 96(t0)
	sd	s9, 104(t0)
	sd	s10, 112(t0)
	sd	s11, 120(t0)
	mv	gp, a0
	ld	x1, 8(gp)
	ld	x2, 16(gp)
	ld	x4, 32(gp)
	ld	x5, 40(gp)
	ld	x6, 48(gp)
	ld	x7, 56(gp)
	ld	x8, 64(gp)
	ld	x9, 72(gp)
	ld	x10, 80(gp)
	ld	x11, 88(gp)
	ld	x12, 96(gp)
	ld	x13, 104(gp)
	ld	x14, 112(gp)
	ld	x15, 120(gp)
	ld	x16, 128(gp)
	ld	x17, 136(gp)
	ld	x18, 144(gp)
	ld	x19, 152(gp)
	ld	x20, 160(gp)
	ld	x21, 168(gp)
	ld	x22, 176(gp)
	ld	x23, 184(gp)
	ld	x24, 192(gp)
	ld	x25, 200(gp)
	ld	x26, 208(gp)
	ld	x27, 216(gp)
	ld	x28, 224(gp)
	ld	x29, 232(gp)
	ld	x30, 240(gp)
	ld	x31, 248(gp)		/* = SNAP_START_PC */
	jr	x31

	.globl window_exit
window_exit:				/* reached from the exit stub planted in the borrowed code */
	la	t0, save_area
	ld	ra, 0(t0)
	ld	sp, 8(t0)
	ld	gp, 16(t0)
	ld	tp, 24(t0)
	ld	s0, 32(t0)
	ld	s1, 40(t0)
	ld	s2, 48(t0)
	ld	s3, 56(t0)
	ld	s4, 64(t0)
	ld	s5, 72(t0)
	ld	s6, 80(t0)
	ld	s7, 88(t0)
	ld	s8, 96(t0)
	ld	s9, 104(t0)
	ld	s10, 112(t0)
	ld	s11, 120(t0)
	ret

	.section .rodata
	.balign 4096
	.globl snap_blob
snap_blob:
	.incbin "snap.bin"
	.balign 4096
	.globl extra_blob
extra_blob:
	.incbin "extra.bin"

	.bss
	.balign 16
save_area:
	.space 128
	.space 65536
mystack_top:

/* 128 KB of nops: run to turn over the instruction cache */
	.text
	.balign 64
	.globl sled
sled:
	.fill 32768, 4, 0x00000013
	ret
