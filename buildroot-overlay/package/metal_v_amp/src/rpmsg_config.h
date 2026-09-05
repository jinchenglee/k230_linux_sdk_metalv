#ifndef METAL_V_RPMSG_CONFIG_H
#define METAL_V_RPMSG_CONFIG_H

#define RL_BUFFER_PAYLOAD_SIZE 496U
#define RL_BUFFER_COUNT 256U
#define RL_API_HAS_ZEROCOPY 1
#define RL_USE_STATIC_API 1
#define RL_USE_DCACHE 1
#define RL_ALLOW_CONSUMED_BUFFERS_NOTIFICATION 1
#define RL_DEBUG_CHECK_BUFFERS 0

#define mb() __asm__ volatile ("fence rw, rw" ::: "memory")

#endif
