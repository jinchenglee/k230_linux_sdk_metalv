#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "amp_shm.h"

#define TEST_TIMEOUT_NS UINT64_C(2000000000)

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static void io_release_fence(void)
{
	__asm__ volatile ("fence rw, w" ::: "memory");
}

static void io_acquire_fence(void)
{
	__asm__ volatile ("fence r, rw" ::: "memory");
}

static uint8_t pattern_byte(uint32_t seed, uint32_t index)
{
	uint32_t value = seed ^ (index * UINT32_C(0x9e3779b9));

	value ^= value >> 16;
	value *= UINT32_C(0x7feb352d);
	value ^= value >> 15;
	return value;
}

static int run_exchange(volatile uint8_t *mapping, uint64_t sequence,
			uint32_t length)
{
	volatile struct amp_shm_request *request = (void *)(mapping +
		AMP_SHM_REQUEST_OFFSET);
	volatile struct amp_shm_publish *request_publish = (void *)(mapping +
		AMP_SHM_REQ_PUB_OFFSET);
	volatile struct amp_shm_response *response = (void *)(mapping +
		AMP_SHM_RESPONSE_OFFSET);
	volatile struct amp_shm_publish *response_publish = (void *)(mapping +
		AMP_SHM_RESP_PUB_OFFSET);
	volatile uint8_t *request_data = mapping + AMP_SHM_REQUEST_DATA;
	volatile uint8_t *response_data = mapping + AMP_SHM_RESPONSE_DATA;
	uint32_t seed = (uint32_t)sequence ^ UINT32_C(0x6d657461);
	uint32_t request_crc, response_crc, i;
	uint64_t started, elapsed;

	for (i = 0; i < length; ++i)
		request_data[i] = pattern_byte(seed, i);
	request_crc = amp_crc32((const uint8_t *)(uintptr_t)request_data, length);
	request->command = AMP_SHM_COMMAND_XOR;
	request->length = length;
	request->crc32 = request_crc;
	request->seed = seed;
	io_release_fence();
	request_publish->sequence = sequence;
	io_release_fence();

	started = monotonic_ns();
	while (response_publish->sequence != sequence) {
		if (monotonic_ns() - started > TEST_TIMEOUT_NS) {
			fprintf(stderr, "timeout: seq=%" PRIu64 " length=%" PRIu32 "\n",
				sequence, length);
			return -1;
		}
	}
	io_acquire_fence();
	elapsed = monotonic_ns() - started;

	if (response->request_sequence != sequence ||
	    response->result != AMP_SHM_OK || response->length != length ||
	    response->request_crc32 != request_crc) {
		fprintf(stderr,
			"bad response: seq=%" PRIu64 " result=%" PRIu32
			" length=%" PRIu32 " request_crc=%08" PRIx32 "\n",
			response->request_sequence, response->result, response->length,
			response->request_crc32);
		return -1;
	}

	for (i = 0; i < length; ++i) {
		uint8_t expected = pattern_byte(seed, i) ^ AMP_SHM_XOR_VALUE;

		if (response_data[i] != expected) {
			fprintf(stderr,
				"data mismatch: seq=%" PRIu64 " offset=%" PRIu32
				" expected=%02x actual=%02x\n",
				sequence, i, expected, response_data[i]);
			return -1;
		}
	}
	response_crc = amp_crc32((const uint8_t *)(uintptr_t)response_data,
				 length);
	if (response_crc != response->response_crc32) {
		fprintf(stderr,
			"response CRC mismatch: expected=%08" PRIx32
			" actual=%08" PRIx32 "\n",
			response->response_crc32, response_crc);
		return -1;
	}

	printf("PASS seq=%" PRIu64 " bytes=%" PRIu32
	       " round_trip=%.3f ms"
	       " big_cycles{invalidate=%" PRIu64 ",request_crc=%" PRIu64
	       ",transform=%" PRIu64 ",response_crc=%" PRIu64
	       ",clean=%" PRIu64 "}\n",
	       sequence, length, elapsed / 1000000.0,
	       response->timing.request_invalidate_cycles,
	       response->timing.request_crc_cycles,
	       response->timing.transform_cycles,
	       response->timing.response_crc_cycles,
	       response->timing.response_clean_cycles);
	return 0;
}

int main(int argc, char **argv)
{
	static const uint32_t sizes[] = {
		1, 63, 64, 65, 4095, 4096, 4097, 65536, AMP_SHM_MAX_PAYLOAD,
	};
	volatile struct amp_shm_header *header;
	volatile uint8_t *mapping;
	uint64_t sequence;
	unsigned long loops = 1;
	char *end = NULL;
	size_t i;
	unsigned long loop;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [loops]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 2) {
		errno = 0;
		loops = strtoul(argv[1], &end, 0);
		if (errno || !end || *end || loops == 0) {
			fprintf(stderr, "invalid loop count: %s\n", argv[1]);
			return EXIT_FAILURE;
		}
	}

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return EXIT_FAILURE;
	}
	mapping = mmap(NULL, AMP_SHM_MAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, (off_t)AMP_SHM_PHYS_BASE);
	close(fd);
	if (mapping == MAP_FAILED) {
		perror("mmap AMP shared memory");
		return EXIT_FAILURE;
	}

	header = (void *)(mapping + AMP_SHM_HEADER_OFFSET);
	header->magic = AMP_SHM_ABI_MAGIC;
	header->version = AMP_SHM_ABI_VERSION;
	header->cache_line = AMP_SHM_CACHE_LINE;
	header->max_payload = AMP_SHM_MAX_PAYLOAD;
	header->generation = monotonic_ns();
	io_release_fence();
	sequence = monotonic_ns();
	if (!sequence)
		sequence = 1;
	sequence ^= header->generation << 1;

	for (loop = 0; loop < loops; ++loop) {
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
			if (run_exchange(mapping, ++sequence, sizes[i]) != 0) {
				munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
				return EXIT_FAILURE;
			}
		}
	}

	printf("AMP shared-memory test passed: %lu loops, %zu exchanges\n",
	       loops, loops * (sizeof(sizes) / sizeof(sizes[0])));
	munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
	return EXIT_SUCCESS;
}
