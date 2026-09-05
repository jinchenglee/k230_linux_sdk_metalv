#include <stdint.h>

#include "cache.h"
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_platform.h"
#include "rpmsg_service.h"

#define K230_RSC_VDEV 3U
#define K230_VIRTIO_ID_RPMSG 7U
#define K230_VIRTIO_RPMSG_F_NS (1U << 0)
#define K230_FW_RSC_ADDR_ANY UINT32_C(0xffffffff)

struct k230_rsc_vring {
    uint32_t da;
    uint32_t align;
    uint32_t num;
    uint32_t notifyid;
    uint32_t pa;
} __attribute__((packed));

struct k230_rpmsg_resource_table {
    uint32_t ver;
    uint32_t num;
    uint32_t reserved[2];
    uint32_t offset[1];
    uint32_t type;
    uint32_t id;
    uint32_t notifyid;
    uint32_t dfeatures;
    uint32_t gfeatures;
    uint32_t config_len;
    uint8_t status;
    uint8_t num_of_vrings;
    uint8_t vdev_reserved[2];
    struct k230_rsc_vring vring[2];
} __attribute__((packed));

static struct rpmsg_lite_instance rpmsg_instance_context;
static struct rpmsg_lite_ept_static_context echo_endpoint_context;
static struct rpmsg_lite_instance *rpmsg_instance;
static struct rpmsg_lite_endpoint *echo_endpoint;
static unsigned long rx_messages;
static unsigned long tx_messages;
static uint32_t announced;

static void resource_table_init(void)
{
    volatile struct k230_rpmsg_resource_table *table =
        (void *)(uintptr_t)K230_RPMSG_RSC_PHYS_BASE;
    struct k230_rpmsg_resource_table initial = {
        .ver = 1,
        .num = 1,
        .offset = { 20 },
        .type = K230_RSC_VDEV,
        .id = K230_VIRTIO_ID_RPMSG,
        .notifyid = K230_FW_RSC_ADDR_ANY,
        .dfeatures = K230_VIRTIO_RPMSG_F_NS,
        .num_of_vrings = 2,
        .vring = {
            { K230_RPMSG_SHMEM_BASE, 0x1000, 256,
              K230_FW_RSC_ADDR_ANY, 0 },
            { K230_RPMSG_SHMEM_BASE + K230_RPMSG_VRING_SIZE,
              0x1000, 256, K230_FW_RSC_ADDR_ANY, 0 },
        },
    };
    uint32_t i;
    const uint8_t *src = (const uint8_t *)&initial;
    volatile uint8_t *dst = (volatile uint8_t *)table;

    for (i = 0; i < sizeof(initial); ++i)
        dst[i] = src[i];
    cache_clean_range((const void *)table, sizeof(initial));
}

static int32_t echo_rx(void *payload, uint32_t payload_len, uint32_t src,
                       void *priv)
{
    int32_t ret;

    (void)priv;
    ++rx_messages;
    ret = rpmsg_lite_send(rpmsg_instance, echo_endpoint, src, payload,
                          payload_len, RL_DONT_BLOCK);
    if (ret == RL_SUCCESS)
        ++tx_messages;
    return RL_RELEASE;
}

int rpmsg_service_init(void)
{
    resource_table_init();
    rpmsg_instance = rpmsg_lite_remote_init(
        (void *)(uintptr_t)K230_RPMSG_SHMEM_BASE,
        RL_PLATFORM_K230_LINK_ID, RL_NO_FLAGS,
        &rpmsg_instance_context);
    if (!rpmsg_instance)
        return -1;
    echo_endpoint = rpmsg_lite_create_ept(rpmsg_instance,
        K230_RPMSG_ENDPOINT, echo_rx, 0, &echo_endpoint_context);
    return echo_endpoint ? 0 : -1;
}

void rpmsg_service_poll(uint32_t mailbox_pending)
{
    if (!rpmsg_instance)
        return;
    if (mailbox_pending)
        rpmsg_platform_poll();
    if (!announced && rpmsg_lite_is_link_up(rpmsg_instance)) {
        if (rpmsg_ns_announce(rpmsg_instance, echo_endpoint,
                              "rpmsg-raw", RL_NS_CREATE) == RL_SUCCESS)
            announced = 1;
    }
}

unsigned long rpmsg_service_rx_count(void) { return rx_messages; }
unsigned long rpmsg_service_tx_count(void) { return tx_messages; }
int rpmsg_service_link_up(void)
{
    return rpmsg_instance && rpmsg_lite_is_link_up(rpmsg_instance);
}
