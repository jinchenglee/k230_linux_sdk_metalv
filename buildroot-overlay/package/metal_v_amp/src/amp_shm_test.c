#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "amp_shm.h"
#include "mailbox.h"

#define TEST_TIMEOUT_NS UINT64_C(2000000000)
#define LATENCY_WARMUP 100UL
#define LATENCY_MAX_SAMPLES 1000000UL

enum notify_mode {
	NOTIFY_POLL,
	NOTIFY_MAILBOX_IRQ,
	NOTIFY_MAILBOX_POLL,
};

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
			uint32_t length, enum notify_mode mode,
			volatile uint8_t *mailbox, int mailbox_fd,
			uint64_t *completion_irq, uint64_t *elapsed_ns, int verbose)
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
	request->flags = mode == NOTIFY_POLL ? 0 : AMP_SHM_REQUEST_F_MAILBOX;
	io_release_fence();
	request_publish->sequence = sequence;
	io_release_fence();
	started = monotonic_ns();
	if (mode == NOTIFY_MAILBOX_IRQ) {
		struct pollfd descriptor = {
			.fd = mailbox_fd,
			.events = POLLIN,
		};
		ssize_t bytes;
		int ready;

		if (write(mailbox_fd, &sequence, sizeof(sequence)) !=
		    (ssize_t)sizeof(sequence)) {
			perror("ring K230 mailbox");
			return -1;
		}
		do {
			ready = poll(&descriptor, 1, TEST_TIMEOUT_NS / 1000000);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0) {
			if (!ready)
				fprintf(stderr,
					"completion IRQ timeout: seq=%" PRIu64
					" length=%" PRIu32 "\n",
					sequence, length);
			else
				perror("poll K230 mailbox");
			return -1;
		}
		bytes = read(mailbox_fd, completion_irq,
			     sizeof(*completion_irq));
		if (bytes != (ssize_t)sizeof(*completion_irq)) {
			if (bytes < 0)
				perror("read K230 mailbox");
			else
				fprintf(stderr, "short K230 mailbox read: %zd\n",
					bytes);
			return -1;
		}
	} else {
		if (mode == NOTIFY_MAILBOX_POLL) {
			*(volatile uint32_t *)(mailbox +
				K230_CPU2DSP_INT_SET0) = 0;
			io_release_fence();
		}
		while (response_publish->sequence != sequence) {
			if (monotonic_ns() - started > TEST_TIMEOUT_NS) {
				fprintf(stderr,
					"response timeout: seq=%" PRIu64
					" length=%" PRIu32 "\n",
					sequence, length);
				return -1;
			}
		}
	}
	io_acquire_fence();
	if (response_publish->sequence != sequence) {
		fprintf(stderr,
			"completion before response publication: expected=%" PRIu64
			" actual=%" PRIu64 "\n",
			sequence, response_publish->sequence);
		return -1;
	}
	elapsed = monotonic_ns() - started;
	if (elapsed_ns)
		*elapsed_ns = elapsed;

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

	if (verbose)
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

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *samples, size_t count,
			   unsigned int percent)
{
	size_t rank = (count * percent + 99U) / 100U;

	if (!rank)
		rank = 1;
	return samples[rank - 1];
}

static int run_latency_benchmark(volatile uint8_t *mapping,
				 uint64_t *sequence, int mailbox_fd,
				 uint64_t *completion_irq, size_t sample_count)
{
	uint64_t *samples;
	long double total = 0;
	size_t i;
	int ret = -1;

	samples = calloc(sample_count, sizeof(*samples));
	if (!samples) {
		perror("allocate latency samples");
		return -1;
	}

	for (i = 0; i < LATENCY_WARMUP + sample_count; ++i) {
		uint64_t elapsed;

		if (run_exchange(mapping, ++*sequence, 0, NOTIFY_MAILBOX_IRQ,
				 NULL, mailbox_fd, completion_irq, &elapsed, 0) != 0)
			goto out;
		if (i >= LATENCY_WARMUP) {
			samples[i - LATENCY_WARMUP] = elapsed;
			total += elapsed;
		}
	}

	qsort(samples, sample_count, sizeof(*samples), compare_u64);
	printf("AMP zero-payload bidirectional-mailbox latency: "
	       "samples=%zu warmup=%lu\n",
	       sample_count, LATENCY_WARMUP);
	printf("round_trip_us min=%.3f p50=%.3f p95=%.3f p99=%.3f "
	       "max=%.3f mean=%.3Lf\n",
	       samples[0] / 1000.0,
	       percentile(samples, sample_count, 50) / 1000.0,
	       percentile(samples, sample_count, 95) / 1000.0,
	       percentile(samples, sample_count, 99) / 1000.0,
	       samples[sample_count - 1] / 1000.0,
	       total / sample_count / 1000.0L);
	printf("Scope: userspace write through big-core service and both mailbox "
	       "IRQs to Linux poll/read completion.\n");
	ret = 0;
out:
	free(samples);
	return ret;
}

int main(int argc, char **argv)
{
	static const uint32_t sizes[] = {
		1, 63, 64, 65, 4095, 4096, 4097, 65536, AMP_SHM_MAX_PAYLOAD,
	};
	volatile struct amp_shm_header *header;
	volatile uint8_t *mapping;
	volatile uint8_t *mailbox = NULL;
	uint64_t sequence;
	uint64_t completion_irq = 0;
	unsigned long loops = 1;
	int latency_mode = 0;
	char *end = NULL;
	size_t i;
	unsigned long loop;
	int fd;
	enum notify_mode mode = NOTIFY_POLL;
	int mailbox_fd = -1;
	int arg = 1;

	if (arg < argc && !strcmp(argv[arg], "--mailbox")) {
		mode = NOTIFY_MAILBOX_IRQ;
		++arg;
	} else if (arg < argc && !strcmp(argv[arg], "--mailbox-poll")) {
		mode = NOTIFY_MAILBOX_POLL;
		++arg;
	} else if (arg < argc && !strcmp(argv[arg], "--latency")) {
		mode = NOTIFY_MAILBOX_IRQ;
		latency_mode = 1;
		loops = 10000;
		++arg;
	}
	if (argc - arg > 1) {
		fprintf(stderr,
			"usage: %s [--mailbox|--mailbox-poll] [loops]\n"
			"       %s --latency [samples]\n",
			argv[0], argv[0]);
		return EXIT_FAILURE;
	}
	if (arg < argc) {
		errno = 0;
		loops = strtoul(argv[arg], &end, 0);
		if (errno || !end || *end || loops == 0 ||
		    (latency_mode && loops > LATENCY_MAX_SAMPLES)) {
			fprintf(stderr, "invalid %s: %s\n",
				latency_mode ? "sample count" : "loop count", argv[arg]);
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
	if (mapping == MAP_FAILED) {
		perror("mmap AMP shared memory");
		close(fd);
		return EXIT_FAILURE;
	}
	if (mode == NOTIFY_MAILBOX_POLL) {
		mailbox = mmap(NULL, K230_MAILBOX_MAP_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, (off_t)K230_MAILBOX_PHYS_BASE);
		if (mailbox == MAP_FAILED) {
			perror("mmap K230 mailbox");
			munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
			close(fd);
			return EXIT_FAILURE;
		}
	}
	close(fd);
	if (mode == NOTIFY_MAILBOX_IRQ) {
		mailbox_fd = open("/dev/k230-amp-mailbox", O_RDWR | O_CLOEXEC);
		if (mailbox_fd < 0) {
			perror("open /dev/k230-amp-mailbox");
			munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
			return EXIT_FAILURE;
		}
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

	if (latency_mode) {
		int status = run_latency_benchmark(mapping, &sequence, mailbox_fd,
						   &completion_irq, loops);

		close(mailbox_fd);
		munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
		return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	for (loop = 0; loop < loops; ++loop) {
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
			if (run_exchange(mapping, ++sequence, sizes[i], mode, mailbox,
					 mailbox_fd, &completion_irq, NULL, 1) != 0) {
				if (mailbox_fd >= 0)
					close(mailbox_fd);
				if (mailbox)
					munmap((void *)(uintptr_t)mailbox,
					       K230_MAILBOX_MAP_SIZE);
				munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
				return EXIT_FAILURE;
			}
		}
	}

	printf("AMP shared-memory %s test passed: %lu loops, %zu exchanges\n",
	       mode == NOTIFY_MAILBOX_IRQ ? "bidirectional-mailbox" :
	       mode == NOTIFY_MAILBOX_POLL ? "request-mailbox/response-poll" :
	       "polling", loops,
	       loops * (sizeof(sizes) / sizeof(sizes[0])));
	if (mode == NOTIFY_MAILBOX_IRQ)
		printf("Linux completion IRQ count: %" PRIu64 "\n", completion_irq);
	if (mailbox_fd >= 0)
		close(mailbox_fd);

	if (mailbox)
		munmap((void *)(uintptr_t)mailbox, K230_MAILBOX_MAP_SIZE);
	munmap((void *)(uintptr_t)mapping, AMP_SHM_MAP_SIZE);
	return EXIT_SUCCESS;
}
