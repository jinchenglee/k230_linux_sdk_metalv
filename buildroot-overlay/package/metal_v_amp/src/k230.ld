OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
	firmware (rwx) : ORIGIN = 0x1c000000, LENGTH = 16M
}

SECTIONS
{
	. = ORIGIN(firmware);
	__firmware_start = .;

	.text : ALIGN(16) {
		KEEP(*(.text.startup))
		*(.text .text.*)
	} > firmware

	.rodata : ALIGN(16) {
		*(.rodata .rodata.*)
	} > firmware

	.data : ALIGN(16) {
		*(.data .data.*)
	} > firmware

	.bss (NOLOAD) : ALIGN(16) {
		__bss_start = .;
		*(.bss .bss.* COMMON)
		__bss_end = .;
	} > firmware

	. = ALIGN(16);
	__stack_bottom = .;
	. += 16K;
	__stack_top = .;
	__firmware_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.eh_frame .eh_frame.*)
	}

	ASSERT(__firmware_end <= ORIGIN(firmware) + LENGTH(firmware),
	       "Metal-V payload exceeds the big-core firmware carveout")
}
