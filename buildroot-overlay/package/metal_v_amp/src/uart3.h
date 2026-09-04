#ifndef METAL_V_UART3_H
#define METAL_V_UART3_H

void uart3_init(void);
void uart3_putc(char ch);
void uart3_puts(const char *text);
char uart3_getc(void);
void uart3_puthex64(unsigned long value);

#endif
