#ifndef METAL_V_RPMSG_CONFIG_H
#define METAL_V_RPMSG_CONFIG_H

#define RL_BUFFER_PAYLOAD_SIZE 496U
#define RL_BUFFER_COUNT 256U
#define RL_API_HAS_ZEROCOPY 1
#define RL_USE_STATIC_API 1
#define RL_USE_DCACHE 1
#define RL_ALLOW_CONSUMED_BUFFERS_NOTIFICATION 1
#define RL_DEBUG_CHECK_BUFFERS 0

/*
 * Bring-up instrumentation: capture the last RPMsg data buffer seen by
 * platform_cache_invalidate(). This sits in the cache-maintenance hot path and
 * re-reads the line it has just invalidated, so keep it off in normal builds.
 */
/*
 * 0 = scan the vrings every main-loop iteration (defensive against a lost
 * doorbell); 1 = scan only when a mailbox interrupt has been observed.
 */
#ifndef K230_RPMSG_EDGE_GATED_POLL
#define K230_RPMSG_EDGE_GATED_POLL 0
#endif

#ifndef K230_RPMSG_DEBUG_CAPTURE
#define K230_RPMSG_DEBUG_CAPTURE 0
#endif

#define mb() __asm__ volatile ("fence rw, rw" ::: "memory")

#endif
