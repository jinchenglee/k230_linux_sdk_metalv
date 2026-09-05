#ifndef METAL_V_RPMSG_SERVICE_H
#define METAL_V_RPMSG_SERVICE_H

#include <stdint.h>

#define K230_RPMSG_RSC_PHYS_BASE UINT64_C(0x1d300000)
#define K230_RPMSG_SHMEM_BASE    UINT64_C(0x1d400000)
#define K230_RPMSG_VRING_SIZE    UINT32_C(0x00008000)
#define K230_RPMSG_ENDPOINT      UINT32_C(30)

int rpmsg_service_init(void);
void rpmsg_service_poll(uint32_t mailbox_pending);
unsigned long rpmsg_service_rx_count(void);
unsigned long rpmsg_service_tx_count(void);
int rpmsg_service_link_up(void);

#endif
