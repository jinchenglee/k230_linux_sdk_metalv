#ifndef AMP_SHARED_BUFFER_POOL_H
#define AMP_SHARED_BUFFER_POOL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define AMP_SHARED_BUFFER_POOL_ABI_VERSION 1U

#define AMP_SHARED_BUFFER_CAP_CONTIGUOUS      (1U << 0)
#define AMP_SHARED_BUFFER_CAP_REMOTE_LINEAR   (1U << 1)
#define AMP_SHARED_BUFFER_CAP_CPU_MMAP_WC     (1U << 2)
#define AMP_SHARED_BUFFER_CAP_NO_CPU_SYNC     (1U << 3)

struct amp_shared_buffer_pool_info {
	__u32 version;
	__u32 capabilities;
	__u64 pool_size;
	__u64 minimum_alignment;
	__u64 remote_base;
};

/* Allocation and DMA mapping happen before streaming. remote_token is opaque
 * to Linux userspace: only the remote platform backend may interpret it. */
struct amp_shared_buffer_alloc {
	__u64 capacity;
	__u64 alignment;
	__u64 remote_token;
	__u64 allocation_id;
	__s32 fd;
	__u32 flags;
};

#define AMP_SHARED_BUFFER_IOC_MAGIC 'A'
#define AMP_SHARED_BUFFER_IOC_GET_INFO \
	_IOR(AMP_SHARED_BUFFER_IOC_MAGIC, 0x40, \
	     struct amp_shared_buffer_pool_info)
#define AMP_SHARED_BUFFER_IOC_ALLOC \
	_IOWR(AMP_SHARED_BUFFER_IOC_MAGIC, 0x41, \
	      struct amp_shared_buffer_alloc)

#endif
