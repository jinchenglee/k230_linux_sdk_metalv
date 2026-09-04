/* Derived from Metal-V's K230 platform and the K230 SDK UART driver. */
#include "uart3.h"

#define UART3_BASE 0x91403000UL
#define UART_CLOCK 50000000UL
#define UART_BAUD 115200UL

#define UART_RBR 0x00
#define UART_THR 0x00
#define UART_DLL 0x00
#define UART_IER 0x04
#define UART_DLH 0x04
#define UART_FCR 0x08
#define UART_IIR 0x08
#define UART_LCR 0x0c
#define UART_MCR 0x10
#define UART_LSR 0x14
#define UART_SCH 0x1c
#define UART_USR 0x7c
#define UART_DLF 0xc0

#define UART_LSR_DR (1U << 0)
#define UART_LSR_THRE (1U << 5)

static inline volatile unsigned int *uart_reg(unsigned long offset)
{
	return (volatile unsigned int *)(UART3_BASE + offset);
}

void uart3_init(void)
{
	unsigned long bdiv = UART_CLOCK / UART_BAUD;
	unsigned int dlh = bdiv >> 12;
	unsigned int dll = (bdiv - ((unsigned long)dlh << 12)) / 16;
	unsigned int dlf = bdiv - ((unsigned long)dlh << 12) - dll * 16;

	*uart_reg(UART_LCR) = 0x00;
	*uart_reg(UART_IER) = 0x00;
	*uart_reg(UART_LCR) = 0x80;
	*uart_reg(UART_DLL) = dll;
	*uart_reg(UART_DLH) = dlh;
	*uart_reg(UART_DLF) = dlf;
	*uart_reg(UART_LCR) = 0x03;
	*uart_reg(UART_FCR) = 0x01;
	*uart_reg(UART_MCR) = 0x00;
	(void)*uart_reg(UART_LSR);
	(void)*uart_reg(UART_RBR);
	(void)*uart_reg(UART_USR);
	(void)*uart_reg(UART_IIR);
	*uart_reg(UART_SCH) = 0x00;
}

void uart3_putc(char ch)
{
	if (ch == '\n') {
		while (!(*uart_reg(UART_LSR) & UART_LSR_THRE))
			;
		*uart_reg(UART_THR) = '\r';
	}
	while (!(*uart_reg(UART_LSR) & UART_LSR_THRE))
		;
	*uart_reg(UART_THR) = (unsigned char)ch;
}

void uart3_puts(const char *text)
{
	while (*text)
		uart3_putc(*text++);
}

char uart3_getc(void)
{
	while (!(*uart_reg(UART_LSR) & UART_LSR_DR))
		;
	return *uart_reg(UART_RBR) & 0xff;
}

int uart3_try_getc(char *ch)
{
	if (!(*uart_reg(UART_LSR) & UART_LSR_DR))
		return 0;
	*ch = *uart_reg(UART_RBR) & 0xff;
	return 1;
}

void uart3_puthex64(unsigned long value)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	uart3_puts("0x");
	for (shift = 60; shift >= 0; shift -= 4)
		uart3_putc(digits[(value >> shift) & 0xf]);
}
