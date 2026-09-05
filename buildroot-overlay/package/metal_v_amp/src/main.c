#include <stdint.h>

#include "amp_shm.h"
#include "cache.h"
#include "mailbox.h"
#include "rpmsg_service.h"
#include "uart3.h"

extern char __firmware_start[];
extern char __firmware_end[];

static unsigned long transactions;
static unsigned long errors;
static unsigned long polling_transactions;

#define SHM_PTR(type, offset) \
	((volatile type *)(uintptr_t)(AMP_SHM_PHYS_BASE + (offset)))

static inline unsigned long read_mhartid(void)
{
	unsigned long value;
	__asm__ volatile ("csrr %0, mhartid" : "=r"(value));
	return value;
}

static inline unsigned long read_misa(void)
{
	unsigned long value;
	__asm__ volatile ("csrr %0, misa" : "=r"(value));
	return value;
}

static inline uint64_t read_cycle(void)
{
	uint64_t value;
	__asm__ volatile ("rdcycle %0" : "=r"(value));
	return value;
}

static void print_info(void)
{
	uart3_puts("mhartid: ");
	uart3_puthex64(read_mhartid());
	uart3_puts("\nmisa:    ");
	uart3_puthex64(read_misa());
	uart3_puts("\nimage:   ");
	uart3_puthex64((unsigned long)__firmware_start);
	uart3_puts(" - ");
	uart3_puthex64((unsigned long)__firmware_end);
	uart3_puts("\nAMP transactions: ");
	uart3_puthex64(transactions);
	uart3_puts("\nAMP errors: ");
	uart3_puthex64(errors);
	uart3_puts("\nAMP mailbox IRQs: ");
	uart3_puthex64(mailbox_interrupt_count());
	uart3_puts("\nAMP poll fallback: ");
	uart3_puthex64(polling_transactions);
	uart3_puts("\nAMP unhandled IRQs: ");
	uart3_puthex64(mailbox_unhandled_count());
	uart3_puts("\nRPMsg link/rx/tx: ");
	uart3_puthex64(rpmsg_service_link_up());
	uart3_puts("/");
	uart3_puthex64(rpmsg_service_rx_count());
	uart3_puts("/");
	uart3_puthex64(rpmsg_service_tx_count());
	uart3_puts("\n");
}

static void publish_response(uint64_t sequence, uint32_t result,
			     uint32_t length, uint32_t request_crc,
			     uint32_t response_crc,
			     const struct amp_shm_timing *timing,
			     uint32_t notify)
{
	volatile struct amp_shm_response *response =
		SHM_PTR(struct amp_shm_response, AMP_SHM_RESPONSE_OFFSET);
	volatile struct amp_shm_publish *publish =
		SHM_PTR(struct amp_shm_publish, AMP_SHM_RESP_PUB_OFFSET);

	response->result = result;
	response->length = length;
	response->request_crc32 = request_crc;
	response->response_crc32 = response_crc;
	response->request_sequence = sequence;
	response->timing.request_invalidate_cycles = timing->request_invalidate_cycles;
	response->timing.request_crc_cycles = timing->request_crc_cycles;
	response->timing.transform_cycles = timing->transform_cycles;
	response->timing.response_crc_cycles = timing->response_crc_cycles;
	response->timing.response_clean_cycles = timing->response_clean_cycles;
	cache_clean_range((const void *)response, sizeof(*response));
	amp_release_fence();
	publish->sequence = sequence;
	cache_clean_range((const void *)publish, sizeof(*publish));
	if (notify)
		mailbox_notify_small_core();
}

static uint64_t service_request(uint64_t last_sequence, uint32_t mailbox_pending)
{
	volatile struct amp_shm_header *header =
		SHM_PTR(struct amp_shm_header, AMP_SHM_HEADER_OFFSET);
	volatile struct amp_shm_request *request =
		SHM_PTR(struct amp_shm_request, AMP_SHM_REQUEST_OFFSET);
	volatile struct amp_shm_publish *publish =
		SHM_PTR(struct amp_shm_publish, AMP_SHM_REQ_PUB_OFFSET);
	uint8_t *request_data =
		(uint8_t *)(uintptr_t)(AMP_SHM_PHYS_BASE + AMP_SHM_REQUEST_DATA);
	uint8_t *response_data =
		(uint8_t *)(uintptr_t)(AMP_SHM_PHYS_BASE + AMP_SHM_RESPONSE_DATA);
	uint64_t sequence;
	uint32_t length, request_crc, response_crc, i;
	struct amp_shm_timing timing = { 0 };
	uint64_t started;

	cache_invalidate_range((const void *)publish, sizeof(*publish));
	sequence = publish->sequence;
	if (!sequence || sequence == last_sequence)
		return last_sequence;

	amp_acquire_fence();
	cache_invalidate_range((const void *)header, sizeof(*header));
	cache_invalidate_range((const void *)request, sizeof(*request));
	if (header->magic != AMP_SHM_ABI_MAGIC ||
	    header->version != AMP_SHM_ABI_VERSION ||
	    header->cache_line != AMP_SHM_CACHE_LINE ||
	    header->max_payload != AMP_SHM_MAX_PAYLOAD) {
		++errors;
		publish_response(sequence, AMP_SHM_BAD_HEADER, 0, 0, 0, &timing,
				 mailbox_pending);
		return sequence;
	}

	if ((request->flags & AMP_SHM_REQUEST_F_MAILBOX) && !mailbox_pending)
		return last_sequence;
	if (!(request->flags & AMP_SHM_REQUEST_F_MAILBOX))
		++polling_transactions;
	length = request->length;
	if (request->command != AMP_SHM_COMMAND_XOR) {
		++errors;
		publish_response(sequence, AMP_SHM_BAD_COMMAND, 0, 0, 0, &timing,
				 mailbox_pending);
		return sequence;
	}
	if (length > AMP_SHM_MAX_PAYLOAD) {
		++errors;
		publish_response(sequence, AMP_SHM_BAD_LENGTH, 0, 0, 0, &timing,
				 mailbox_pending);
		return sequence;
	}

	started = read_cycle();
	cache_invalidate_range(request_data, length);
	timing.request_invalidate_cycles = read_cycle() - started;
	started = read_cycle();
	request_crc = amp_crc32(request_data, length);
	timing.request_crc_cycles = read_cycle() - started;
	if (request_crc != request->crc32) {
		++errors;
		publish_response(sequence, AMP_SHM_BAD_CRC, length,
				 request_crc, 0, &timing, mailbox_pending);
		return sequence;
	}

	started = read_cycle();
	for (i = 0; i < length; ++i)
		response_data[i] = request_data[i] ^ AMP_SHM_XOR_VALUE;
	timing.transform_cycles = read_cycle() - started;
	started = read_cycle();
	response_crc = amp_crc32(response_data, length);
	timing.response_crc_cycles = read_cycle() - started;
	started = read_cycle();
	cache_clean_range(response_data, length);
	timing.response_clean_cycles = read_cycle() - started;
	amp_release_fence();
	publish_response(sequence, AMP_SHM_OK, length, request_crc,
			 response_crc, &timing, mailbox_pending);
	++transactions;
	return sequence;
}

void main(void)
{
	volatile struct amp_shm_publish *request_publish =
		SHM_PTR(struct amp_shm_publish, AMP_SHM_REQ_PUB_OFFSET);
	volatile struct amp_shm_publish *response_publish =
		SHM_PTR(struct amp_shm_publish, AMP_SHM_RESP_PUB_OFFSET);
	uint64_t last_sequence;
	char ch;

	uart3_init();
	/* Shared RAM survives a payload restart; do not replay an old request. */
	cache_invalidate_range((const void *)request_publish,
			       sizeof(*request_publish));
	mailbox_init();
	if (rpmsg_service_init())
		uart3_puts("RPMsg-Lite initialization failed.\n");
	last_sequence = request_publish->sequence;
	response_publish->sequence = 0;
	cache_clean_range((const void *)response_publish,
			  sizeof(*response_publish));
	amp_release_fence();
	uart3_puts("\nMetal-V K230 AMP console\n");
	uart3_puts("big-core UART3 is alive\n");
	print_info();
	uart3_puts("Shared-memory service is ready at 0x1d000000.\n");
	uart3_puts("Mailbox IRQ 109 is enabled; polling fallback remains available.\n");
	uart3_puts("Type characters to test RX; CR prints core info.\n");
	uart3_puts("metal-v> ");

	for (;;) {
		uint32_t mailbox_pending = mailbox_take_pending();

		last_sequence = service_request(last_sequence, mailbox_pending);
		rpmsg_service_poll(mailbox_pending);
		if (!uart3_try_getc(&ch))
			continue;
		if (ch == '\r' || ch == '\n') {
			uart3_puts("\n");
			print_info();
			uart3_puts("metal-v> ");
		} else {
			uart3_putc(ch);
		}
	}
}
