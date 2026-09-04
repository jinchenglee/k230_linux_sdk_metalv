#ifndef METAL_V_CACHE_H
#define METAL_V_CACHE_H

#include <stddef.h>

void cache_clean_range(const void *address, size_t length);
void cache_invalidate_range(const void *address, size_t length);

static inline void amp_release_fence(void)
{
	__asm__ volatile ("fence rw, w" ::: "memory");
}

static inline void amp_acquire_fence(void)
{
	__asm__ volatile ("fence r, rw" ::: "memory");
}

#endif
