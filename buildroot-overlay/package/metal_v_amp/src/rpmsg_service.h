#ifndef METAL_V_RPMSG_SERVICE_H
#define METAL_V_RPMSG_SERVICE_H

#include <stdint.h>

#define K230_RPMSG_RSC_PHYS_BASE UINT64_C(0x1d300000)
#define K230_RPMSG_SHMEM_BASE    UINT64_C(0x1d400000)
#define K230_RPMSG_VRING_SIZE    UINT32_C(0x00008000)
#define K230_RPMSG_ENDPOINT      UINT32_C(30)
/* Diagnostics block in the free area of the AMP shm page (0x140..0xfff).
 * The big-core UART drops characters under sustained output, so counters are
 * published here and read from Linux via /dev/mem instead. */
#define K230_RPMSG_STATS_OFFSET  UINT32_C(0x0200)
#define K230_RPMSG_STATS_MAGIC   UINT32_C(0x52535431)

struct k230_rpmsg_stats {
    uint32_t magic;
    uint32_t link_up;
    uint32_t rx_callbacks;   /* echo_rx invocations */
    uint32_t tx_sent;        /* successful rpmsg_lite_send */
    uint32_t tx_failed;      /* rpmsg_lite_send returned non-success */
    uint32_t fetch_rx;       /* buffers pulled off the receive vq */
    uint32_t fetch_tx;       /* buffers pulled off the transmit vq */
    uint32_t rvq_avail_idx;  /* avail->idx Linux has published */
    uint32_t rvq_consumed;   /* vq_available_idx this core has consumed */
    uint32_t tvq_avail_idx;
    uint32_t tvq_consumed;
    uint32_t reserved[5];
};

#define K230_RPMSG_BUFFER_BASE   UINT64_C(0x1d500000)
#define K230_RPMSG_BUFFER_SIZE   UINT32_C(0x00040000)

int rpmsg_service_init(void);
void rpmsg_service_poll(uint32_t mailbox_pending);
unsigned long rpmsg_service_rx_count(void);
unsigned long rpmsg_service_tx_count(void);
int rpmsg_service_link_up(void);
void rpmsg_service_publish_stats(void);
uint32_t rpmsg_service_driver_ok(void);
uint32_t rpmsg_service_rsc_status(void);
void rpmsg_service_print_debug(void);

#endif
