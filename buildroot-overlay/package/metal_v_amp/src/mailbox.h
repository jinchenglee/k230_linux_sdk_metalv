#ifndef METAL_V_AMP_MAILBOX_H
#define METAL_V_AMP_MAILBOX_H

#include <stdint.h>

#define K230_MAILBOX_PHYS_BASE UINT64_C(0x91104000)
#define K230_MAILBOX_MAP_SIZE  UINT32_C(0x1000)
#define K230_CPU2DSP_INT_EN0     UINT32_C(0x0000)
#define K230_CPU2DSP_INT_SET0    UINT32_C(0x0004)
#define K230_CPU2DSP_INT_CLEAR0  UINT32_C(0x0008)
#define K230_CPU2DSP_INT_STATUS0 UINT32_C(0x000c)
#define K230_DSP2CPU_INT_EN0     UINT32_C(0x0014)
#define K230_DSP2CPU_INT_SET0    UINT32_C(0x0018)
#define K230_DSP2CPU_INT_CLEAR0  UINT32_C(0x001c)
#define K230_DSP2CPU_INT_STATUS0 UINT32_C(0x0020)
#define K230_MAILBOX_ENABLE  ((UINT32_C(1) << 16) | (UINT32_C(1) << 0))
#define K230_MAILBOX_BIG_IRQ UINT32_C(109)

void mailbox_init(void);
void mailbox_notify_small_core(void);
uint32_t mailbox_take_pending(void);
unsigned long mailbox_interrupt_count(void);
unsigned long mailbox_unhandled_count(void);
void mailbox_handle_trap(uint64_t cause, uint64_t epc, uint64_t value);

#endif
