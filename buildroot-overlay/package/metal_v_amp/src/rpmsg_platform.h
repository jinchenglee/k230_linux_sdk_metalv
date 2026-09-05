#ifndef METAL_V_RPMSG_PLATFORM_H
#define METAL_V_RPMSG_PLATFORM_H

#include <stdint.h>
#include "virtqueue.h"

#define VRING_ALIGN 0x1000U
#define VRING_SIZE  0x8000U
#define RL_VRING_OVERHEAD (2UL * VRING_SIZE)

#define RL_PLATFORM_MAX_ISR_COUNT 2U
#define RL_GET_VQ_ID(link_id, queue_id) ((queue_id) & 1U)
#define RL_GET_LINK_ID(id) 0U
#define RL_GET_Q_ID(id) ((id) & 1U)
#define RL_PLATFORM_K230_LINK_ID 0U
#define RL_PLATFORM_HIGHEST_LINK_ID 0U

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data);
int32_t platform_deinit_interrupt(uint32_t vector_id);
int32_t platform_interrupt_enable(uint32_t vector_id);
int32_t platform_interrupt_disable(uint32_t vector_id);
int32_t platform_in_isr(void);
void platform_notify(uint32_t vector_id);
void platform_time_delay(uint32_t num_msec);
void platform_map_mem_region(uint32_t va, uint32_t pa, uint32_t size,
                             uint32_t flags);
void platform_cache_all_flush_invalidate(void);
void platform_cache_disable(void);
void platform_cache_invalidate(void *data, uint32_t len);
void platform_cache_flush(void *data, uint32_t len);
uintptr_t platform_vatopa(void *addr);
void *platform_patova(uintptr_t addr);
int32_t platform_init(void);
int32_t platform_deinit(void);
void rpmsg_platform_poll(void);
unsigned long virtqueue_k230_fetch_count(uint32_t queue);
uint32_t virtqueue_k230_first_desc_len(uint32_t i);
uint32_t virtqueue_k230_first_desc_idx(uint32_t i);
uintptr_t rpmsg_platform_last_buffer(void);
uint32_t rpmsg_platform_last_buffer_word(uint32_t index);

#endif
