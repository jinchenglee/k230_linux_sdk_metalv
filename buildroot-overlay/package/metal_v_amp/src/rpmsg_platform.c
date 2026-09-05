#include <stdint.h>

#include "cache.h"
#include "mailbox.h"
#include "rpmsg_config.h"
#include "rpmsg_env.h"
#include "rpmsg_platform.h"

#define K230_RPMSG_VRING_BASE UINT64_C(0x1d400000)
#define K230_RPMSG_VRING_END  UINT64_C(0x1d410000)

static uintptr_t last_buffer;
static uint32_t last_buffer_words[4];

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
#if K230_RPMSG_DEBUG_CAPTURE
    uintptr_t address = (uintptr_t)data;
    uint32_t words;
    uint32_t i;
#endif

    cache_invalidate_range(data, len);
#if K230_RPMSG_DEBUG_CAPTURE
    /* Avail/used rings are also invalidated through this hook. Record only
     * an external RPMsg data buffer, after it is safe for this core to read. */
    if ((address >= K230_RPMSG_VRING_BASE) &&
        (address < K230_RPMSG_VRING_END))
        return;
    last_buffer = address;
    words = len / sizeof(uint32_t);
    if (words > 4U)
        words = 4U;
    for (i = 0; i < words; ++i)
        last_buffer_words[i] = ((const uint32_t *)data)[i];
    for (; i < 4U; ++i)
        last_buffer_words[i] = 0U;
#endif
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

uintptr_t rpmsg_platform_last_buffer(void)
{
    return last_buffer;
}

uint32_t rpmsg_platform_last_buffer_word(uint32_t index)
{
    return index < 4U ? last_buffer_words[index] : 0U;
}
