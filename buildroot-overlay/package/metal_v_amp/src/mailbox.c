#include <stdint.h>

#include "mailbox.h"

#define K230_PLIC_BASE          UINT64_C(0x0f00000000)
#define K230_PLIC_PRIORITY(irq) (K230_PLIC_BASE + ((uint64_t)(irq) * 4U))
#define K230_PLIC_M_ENABLE      (K230_PLIC_BASE + UINT64_C(0x2000))
#define K230_PLIC_M_THRESHOLD   (K230_PLIC_BASE + UINT64_C(0x200000))
#define K230_PLIC_M_CLAIM       (K230_PLIC_BASE + UINT64_C(0x200004))
#define K230_PLIC_S_PER         (K230_PLIC_BASE + UINT64_C(0x1ffffc))

#define MCAUSE_INTERRUPT        (UINT64_C(1) << 63)
#define MCAUSE_MACHINE_EXTERNAL UINT64_C(11)
#define MSTATUS_MIE             (UINT64_C(1) << 3)
#define MIE_MEIE                (UINT64_C(1) << 11)

static volatile uint32_t pending;
static volatile unsigned long interrupts;
static volatile unsigned long unhandled;

static inline volatile uint32_t *reg32(uint64_t address)
{
	return (volatile uint32_t *)(uintptr_t)address;
}

static inline void write32(uint64_t address, uint32_t value)
{
	*reg32(address) = value;
}

static inline uint32_t read32(uint64_t address)
{
	return *reg32(address);
}

static inline void mailbox_write(uint32_t offset, uint32_t value)
{
	write32(K230_MAILBOX_PHYS_BASE + offset, value);
}

void mailbox_init(void)
{
	uint64_t enable_address = K230_PLIC_M_ENABLE +
		(K230_MAILBOX_BIG_IRQ / 32U) * sizeof(uint32_t);
	uint32_t enable_mask = UINT32_C(1) << (K230_MAILBOX_BIG_IRQ % 32U);
	uint32_t value;

	/* boot_baremetal enters M-mode; route this PLIC instance to M context 0. */
	write32(K230_PLIC_S_PER, 0);
	mailbox_write(K230_CPU2DSP_INT_CLEAR0, 0);
	mailbox_write(K230_CPU2DSP_INT_EN0, K230_MAILBOX_ENABLE);
	mailbox_write(K230_DSP2CPU_INT_EN0, K230_MAILBOX_ENABLE);
	write32(K230_PLIC_PRIORITY(K230_MAILBOX_BIG_IRQ), 1);
	value = read32(enable_address);
	write32(enable_address, value | enable_mask);
	write32(K230_PLIC_M_THRESHOLD, 0);
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	__asm__ volatile ("csrs mie, %0" :: "r"(MIE_MEIE) : "memory");
	__asm__ volatile ("csrs mstatus, %0" :: "r"(MSTATUS_MIE) : "memory");
}
void mailbox_notify_small_core(void)
{
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	mailbox_write(K230_DSP2CPU_INT_SET0, 0);
}


uint32_t mailbox_take_pending(void)
{
	uint64_t old_status;
	uint32_t value;

	__asm__ volatile ("csrrc %0, mstatus, %1"
			  : "=r"(old_status) : "r"(MSTATUS_MIE) : "memory");
	value = pending;
	pending = 0;
	if (old_status & MSTATUS_MIE)
		__asm__ volatile ("csrs mstatus, %0"
				  :: "r"(MSTATUS_MIE) : "memory");
	return value;
}

unsigned long mailbox_interrupt_count(void)
{
	return interrupts;
}

unsigned long mailbox_unhandled_count(void)
{
	return unhandled;
}

void mailbox_handle_trap(uint64_t cause, uint64_t epc, uint64_t value)
{
	uint32_t irq;

	(void)epc;
	(void)value;
	if (cause != (MCAUSE_INTERRUPT | MCAUSE_MACHINE_EXTERNAL)) {
		++unhandled;
		return;
	}
	while ((irq = read32(K230_PLIC_M_CLAIM)) != 0) {
		if (irq == K230_MAILBOX_BIG_IRQ) {
			mailbox_write(K230_CPU2DSP_INT_CLEAR0, 0);
			__asm__ volatile ("fence iorw, iorw" ::: "memory");
			pending = 1;
			++interrupts;
		} else {
			++unhandled;
		}
		write32(K230_PLIC_M_CLAIM, irq);
	}
}
