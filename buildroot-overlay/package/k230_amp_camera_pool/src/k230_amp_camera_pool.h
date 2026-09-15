#ifndef K230_AMP_CAMERA_POOL_H
#define K230_AMP_CAMERA_POOL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define K230_AMP_CAMERA_POOL_ABI_VERSION 1U
#define K230_AMP_CAMERA_POOL_BASE 0x1da00000ULL
#define K230_AMP_CAMERA_BUFFER_SIZE 0x00200000ULL
#define K230_AMP_CAMERA_BUFFER_COUNT 6U
#define K230_AMP_CAMERA_POOL_SIZE \
	(K230_AMP_CAMERA_BUFFER_SIZE * K230_AMP_CAMERA_BUFFER_COUNT)

struct k230_amp_camera_pool_info {
	__u32 version;
	__u32 buffer_count;
	__u64 buffer_size;
	__u64 physical_base;
	__u64 pool_size;
};

struct k230_amp_camera_pool_buffer {
	__u32 id;
	__u32 flags;
	__u64 physical;
	__u64 capacity;
	__s32 fd;
	__u32 reserved;
};

#define K230_AMP_CAMERA_IOC_MAGIC 'K'
#define K230_AMP_CAMERA_IOC_GET_INFO \
	_IOR(K230_AMP_CAMERA_IOC_MAGIC, 0x40, \
	     struct k230_amp_camera_pool_info)
#define K230_AMP_CAMERA_IOC_GET_BUFFER \
	_IOWR(K230_AMP_CAMERA_IOC_MAGIC, 0x41, \
	      struct k230_amp_camera_pool_buffer)

#endif
