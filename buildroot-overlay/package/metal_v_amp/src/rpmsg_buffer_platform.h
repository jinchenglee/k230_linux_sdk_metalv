#ifndef METAL_V_RPMSG_BUFFER_PLATFORM_H
#define METAL_V_RPMSG_BUFFER_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "cache.h"
#include "rpmsg_protocol.h"

/* K230 backend for the generic registered-buffer protocol. Linux reports a
 * remote-visible token; K230 has an identity mapping for this reserved DRAM.
 * Other AMP platforms replace this validator/translator, not the service. */
#define K230_CAMERA_REMOTE_BASE UINT64_C(0x1da00000)
#define K230_CAMERA_REMOTE_SIZE UINT64_C(0x00c00000)

static inline uint16_t rpmsg_platform_resolve_buffer(
	uint64_t remote_token, uint64_t capacity, uintptr_t *local_address)
{
	const uint64_t end = K230_CAMERA_REMOTE_BASE + K230_CAMERA_REMOTE_SIZE;

	if (remote_token < K230_CAMERA_REMOTE_BASE ||
	    capacity > K230_CAMERA_REMOTE_SIZE ||
	    remote_token > end - capacity ||
	    (remote_token % K230_PAYLOAD_CACHE_LINE) != 0)
		return K230_RPMSG_STATUS_INVALID_RANGE;
	*local_address = (uintptr_t)remote_token;
	return K230_RPMSG_STATUS_OK;
}

/* This boundary stays inline so portability does not add a per-frame call or
 * dispatch. K230 is non-coherent: device writes must be invalidated before
 * the remote reads. A coherent platform can replace this with its barrier. */
static inline void rpmsg_platform_acquire_buffer(const void *local_address,
					  size_t length)
{
	cache_invalidate_range(local_address, length);
	amp_acquire_fence();
}

#endif
