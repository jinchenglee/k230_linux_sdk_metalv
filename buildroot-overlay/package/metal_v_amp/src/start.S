	.section .text.startup
	.globl _start
	.type _start, @function
_start:
	la sp, __stack_top

	/* C908 cache/control setup used by the K230 SDK and Metal-V. */
	li t0, 0x11ff
	csrw 0x7c1, t0
	li t0, 0x70013
	csrw 0x7c2, t0
	li t0, 0xe0410009
	csrw 0x7c3, t0
	li t0, 0x16e30c
	csrw 0x7c5, t0

	/* Mark vector state dirty before executing RVV code. */
	li t0, 0x600
	csrs mstatus, t0

	la t0, trap_entry
	csrw mtvec, t0

	la t0, __bss_start
	la t1, __bss_end
1:
	bgeu t0, t1, 2f
	sd zero, 0(t0)
	addi t0, t0, 8
	j 1b
2:
	fence rw, rw
	call main
3:
	wfi
	j 3b

	.size _start, . - _start

	.balign 16
	.globl trap_entry
	.type trap_entry, @function
trap_entry:
	addi sp, sp, -256
	sd ra,   0(sp)
	sd gp,   8(sp)
	sd tp,  16(sp)
	sd t0,  24(sp)
	sd t1,  32(sp)
	sd t2,  40(sp)
	sd s0,  48(sp)
	sd s1,  56(sp)
	sd a0,  64(sp)
	sd a1,  72(sp)
	sd a2,  80(sp)
	sd a3,  88(sp)
	sd a4,  96(sp)
	sd a5, 104(sp)
	sd a6, 112(sp)
	sd a7, 120(sp)
	sd s2, 128(sp)
	sd s3, 136(sp)
	sd s4, 144(sp)
	sd s5, 152(sp)
	sd s6, 160(sp)
	sd s7, 168(sp)
	sd s8, 176(sp)
	sd s9, 184(sp)
	sd s10, 192(sp)
	sd s11, 200(sp)
	sd t3, 208(sp)
	sd t4, 216(sp)
	sd t5, 224(sp)
	sd t6, 232(sp)
	csrr a0, mcause
	csrr a1, mepc
	csrr a2, mtval
	call mailbox_handle_trap
	ld ra,   0(sp)
	ld gp,   8(sp)
	ld tp,  16(sp)
	ld t0,  24(sp)
	ld t1,  32(sp)
	ld t2,  40(sp)
	ld s0,  48(sp)
	ld s1,  56(sp)
	ld a0,  64(sp)
	ld a1,  72(sp)
	ld a2,  80(sp)
	ld a3,  88(sp)
	ld a4,  96(sp)
	ld a5, 104(sp)
	ld a6, 112(sp)
	ld a7, 120(sp)
	ld s2, 128(sp)
	ld s3, 136(sp)
	ld s4, 144(sp)
	ld s5, 152(sp)
	ld s6, 160(sp)
	ld s7, 168(sp)
	ld s8, 176(sp)
	ld s9, 184(sp)
	ld s10, 192(sp)
	ld s11, 200(sp)
	ld t3, 208(sp)
	ld t4, 216(sp)
	ld t5, 224(sp)
	ld t6, 232(sp)
	addi sp, sp, 256
	mret

	.size trap_entry, . - trap_entry
