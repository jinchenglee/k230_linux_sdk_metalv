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
	/* A trap is fatal in this first payload; retain CSRs for a debugger. */
	csrr t0, mcause
	csrr t1, mepc
	csrr t2, mtval
4:
	wfi
	j 4b

	.size trap_entry, . - trap_entry
