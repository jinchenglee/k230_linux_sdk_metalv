#include <stdint.h>

#include "cache.h"
#include "mailbox.h"
#include "rpmsg_env.h"
#include "rpmsg_platform.h"

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
    if (vector_id >= RL_PLATFORM_MAX_ISR_COUNT)
        return -1;
    env_register_isr(vector_id, isr_data);
    return 0;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
    env_unregister_isr(vector_id);
    return 0;
}

int32_t platform_interrupt_enable(uint32_t vector_id)
{
    (void)vector_id;
    return 0;
}

int32_t platform_interrupt_disable(uint32_t vector_id)
{
    (void)vector_id;
    return 0;
}

int32_t platform_in_isr(void)
{
    return 0;
}

void platform_notify(uint32_t vector_id)
{
    (void)vector_id;
    mailbox_notify_small_core();
}

void rpmsg_platform_poll(void)
{
    env_isr(0U);
    env_isr(1U);
}

void platform_time_delay(uint32_t num_msec)
{
    uint64_t cycles = (uint64_t)num_msec * UINT64_C(1600000);
    uint64_t start;

    __asm__ volatile ("rdcycle %0" : "=r"(start));
    for (;;) {
        uint64_t now;
        __asm__ volatile ("rdcycle %0" : "=r"(now));
        if (now - start >= cycles)
            break;
    }
}

void platform_map_mem_region(uint32_t va, uint32_t pa, uint32_t size,
                             uint32_t flags)
{
    (void)va;
    (void)pa;
    (void)size;
    (void)flags;
}

void platform_cache_all_flush_invalidate(void) {}
void platform_cache_disable(void) {}

void platform_cache_invalidate(void *data, uint32_t len)
{
    cache_invalidate_range(data, len);
}

void platform_cache_flush(void *data, uint32_t len)
{
    cache_clean_range(data, len);
}

uintptr_t platform_vatopa(void *addr)
{
    return (uintptr_t)addr;
}

void *platform_patova(uintptr_t addr)
{
    return (void *)addr;
}

int32_t platform_init(void)
{
    return 0;
}

int32_t platform_deinit(void)
{
    return 0;
}
