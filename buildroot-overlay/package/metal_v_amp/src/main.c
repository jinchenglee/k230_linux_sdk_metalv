#include "uart3.h"

extern char __firmware_start[];
extern char __firmware_end[];

static inline unsigned long read_mhartid(void)
{
	unsigned long value;
	__asm__ volatile ("csrr %0, mhartid" : "=r" (value));
	return value;
}

static inline unsigned long read_misa(void)
{
	unsigned long value;
	__asm__ volatile ("csrr %0, misa" : "=r" (value));
	return value;
}

static void print_info(void)
{
	uart3_puts("mhartid: ");
	uart3_puthex64(read_mhartid());
	uart3_puts("\nmisa:    ");
	uart3_puthex64(read_misa());
	uart3_puts("\nimage:   ");
	uart3_puthex64((unsigned long)__firmware_start);
	uart3_puts(" - ");
	uart3_puthex64((unsigned long)__firmware_end);
	uart3_puts("\n");
}

void main(void)
{
	char ch;

	uart3_init();
	uart3_puts("\nMetal-V K230 AMP console\n");
	uart3_puts("big-core UART3 is alive\n");
	print_info();
	uart3_puts("Type characters to test RX; CR prints core info.\n");
	uart3_puts("metal-v> ");

	for (;;) {
		ch = uart3_getc();
		if (ch == '\r' || ch == '\n') {
			uart3_puts("\n");
			print_info();
			uart3_puts("metal-v> ");
		} else {
			uart3_putc(ch);
		}
	}
}
