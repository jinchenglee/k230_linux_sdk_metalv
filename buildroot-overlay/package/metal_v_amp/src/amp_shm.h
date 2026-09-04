#ifndef METAL_V_AMP_SHM_H
#define METAL_V_AMP_SHM_H

#include <stdint.h>

#define AMP_SHM_PHYS_BASE       UINT64_C(0x1d000000)
#define AMP_SHM_ABI_MAGIC       UINT32_C(0x4d56414d) /* "MVAM" */
#define AMP_SHM_ABI_VERSION     UINT32_C(3)
#define AMP_SHM_CACHE_LINE      UINT32_C(64)
#define AMP_SHM_MAX_PAYLOAD     UINT32_C(0x00100000)

#define AMP_SHM_HEADER_OFFSET   UINT32_C(0x0000)
#define AMP_SHM_REQUEST_OFFSET  UINT32_C(0x0040)
#define AMP_SHM_REQ_PUB_OFFSET  UINT32_C(0x0080)
#define AMP_SHM_RESPONSE_OFFSET UINT32_C(0x00c0)
#define AMP_SHM_RESP_PUB_OFFSET UINT32_C(0x0100)
#define AMP_SHM_REQUEST_DATA    UINT32_C(0x1000)
#define AMP_SHM_RESPONSE_DATA   UINT32_C(0x101000)
#define AMP_SHM_MAP_SIZE        UINT32_C(0x201000)

#define AMP_SHM_COMMAND_XOR     UINT32_C(1)
#define AMP_SHM_XOR_VALUE       UINT32_C(0xa5)

#define AMP_SHM_REQUEST_F_MAILBOX UINT32_C(1)
enum amp_shm_result {
	AMP_SHM_OK = 0,
	AMP_SHM_BAD_HEADER = 1,
	AMP_SHM_BAD_COMMAND = 2,
	AMP_SHM_BAD_LENGTH = 3,
	AMP_SHM_BAD_CRC = 4,
};

struct amp_shm_header {
	uint32_t magic;
	uint32_t version;
	uint32_t cache_line;
	uint32_t max_payload;
	uint64_t generation;
	uint8_t reserved[40];
} __attribute__((aligned(64)));

struct amp_shm_request {
	uint32_t command;
	uint32_t length;
	uint32_t crc32;
	uint32_t seed;
	uint32_t flags;
	uint8_t reserved[44];
} __attribute__((aligned(64)));

struct amp_shm_publish {
	uint64_t sequence;
	uint8_t reserved[56];
} __attribute__((aligned(64)));

struct amp_shm_timing {
	uint64_t request_invalidate_cycles;
	uint64_t request_crc_cycles;
	uint64_t transform_cycles;
	uint64_t response_crc_cycles;
	uint64_t response_clean_cycles;
};

struct amp_shm_response {
	uint32_t result;
	uint32_t length;
	uint32_t request_crc32;
	uint32_t response_crc32;
	uint64_t request_sequence;
	struct amp_shm_timing timing;
} __attribute__((aligned(64)));

_Static_assert(sizeof(struct amp_shm_header) == AMP_SHM_CACHE_LINE,
	       "AMP header must occupy one cache line");
_Static_assert(sizeof(struct amp_shm_request) == AMP_SHM_CACHE_LINE,
	       "AMP request must occupy one cache line");
_Static_assert(sizeof(struct amp_shm_publish) == AMP_SHM_CACHE_LINE,
	       "AMP publication must occupy one cache line");
_Static_assert(sizeof(struct amp_shm_response) == AMP_SHM_CACHE_LINE,
	       "AMP response must occupy one cache line");

static inline uint32_t amp_crc32(const uint8_t *data, uint32_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	uint32_t i;

	while (length--) {
		crc ^= *data++;
		for (i = 0; i < 8; ++i)
			crc = (crc >> 1) ^
			      (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
	}
	return ~crc;
}

#endif
