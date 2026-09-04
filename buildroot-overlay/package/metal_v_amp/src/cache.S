	.section .text
	.balign 4
	.globl cache_clean_range
	.type cache_clean_range, @function
cache_clean_range:
	beqz a1, 2f
	add a1, a0, a1
	andi a0, a0, -64
1:
	/* dcache.cpa a0: K230 U-Boot's physical range-clean operation. */
	.long 0x0295000b
	addi a0, a0, 64
	bltu a0, a1, 1b
	/* sync.is, paired with physical range operations by K230 U-Boot. */
	.long 0x01b0000b
2:
	ret
	.size cache_clean_range, . - cache_clean_range

	.balign 4
	.globl cache_invalidate_range
	.type cache_invalidate_range, @function
cache_invalidate_range:
	beqz a1, 2f
	add a1, a0, a1
	andi a0, a0, -64
1:
	/* dcache.ipa a0: K230 U-Boot's physical range-invalidate operation. */
	.long 0x02a5000b
	addi a0, a0, 64
	bltu a0, a1, 1b
	.long 0x01b0000b
2:
	ret
	.size cache_invalidate_range, . - cache_invalidate_range
