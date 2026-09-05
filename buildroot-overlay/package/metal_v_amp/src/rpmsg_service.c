#include <stdint.h>

#include "cache.h"
#include "rpmsg_config.h"
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_platform.h"
#include "amp_shm.h"
#include "rpmsg_service.h"
#include "uart3.h"

#define K230_RSC_VDEV 3U
#define K230_VIRTIO_ID_RPMSG 7U
#define K230_VIRTIO_RPMSG_F_NS (1U << 0)
#define K230_FW_RSC_ADDR_ANY UINT32_C(0xffffffff)
/* virtio_config.h: the driver sets DRIVER_OK once the vrings are usable. */
#define K230_VIRTIO_S_DRIVER_OK UINT8_C(0x04)
#define K230_VIRTIO_S_FAILED    UINT8_C(0x80)

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
static unsigned long tx_failures;
static uint32_t announced;
static uint32_t driver_ok;

/*
 * The vring contents are only meaningful once Linux has attached and set
 * DRIVER_OK. Scanning (or announcing into) the rings before that point acts on
 * uninitialized carveout memory: it can raise link_up from garbage and burn the
 * one-shot name-service announce into a vring Linux has not set up yet, after
 * which /dev/rpmsg0 never appears. Poll only after the driver says it is ready.
 */
static uint32_t rsc_driver_ok(void)
{
    volatile struct k230_rpmsg_resource_table *table =
        (void *)(uintptr_t)K230_RPMSG_RSC_PHYS_BASE;
    uint8_t status;

    if (driver_ok)
        return 1U;
    cache_invalidate_range((const void *)&table->status,
                           sizeof(table->status));
    status = table->status;
    if ((status & K230_VIRTIO_S_DRIVER_OK) &&
        !(status & K230_VIRTIO_S_FAILED))
        driver_ok = 1U;
    return driver_ok;
}

uint32_t rpmsg_service_driver_ok(void) { return rsc_driver_ok(); }

uint32_t rpmsg_service_rsc_status(void)
{
    volatile struct k230_rpmsg_resource_table *table =
        (void *)(uintptr_t)K230_RPMSG_RSC_PHYS_BASE;

    cache_invalidate_range((const void *)&table->status,
                           sizeof(table->status));
    return table->status;
}

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
    else
        ++tx_failures;
    return RL_RELEASE;
}

/*
 * This core is the virtio device for both vrings, so it owns used->flags.
 * Nothing else initializes it: remoteproc does not zero a fixed-DA carveout
 * vring, and rpmsg_lite_remote_init() never calls vq_ring_init() (only the
 * master path does). Stale DRAM with VRING_USED_F_NO_NOTIFY set makes Linux
 * virtqueue_kick() a silent no-op. Clear the bit without disturbing used->idx
 * or the used ring, which may already be live.
 */
static void used_ring_allow_notify(struct virtqueue *vq)
{
    if (!vq)
        return;
    vq->vq_ring.used->flags = 0U;
    cache_clean_range((const void *)&vq->vq_ring.used->flags,
                      sizeof(vq->vq_ring.used->flags));
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
    used_ring_allow_notify(rpmsg_instance->rvq);
    used_ring_allow_notify(rpmsg_instance->tvq);
    echo_endpoint = rpmsg_lite_create_ept(rpmsg_instance,
        K230_RPMSG_ENDPOINT, echo_rx, 0, &echo_endpoint_context);
    return echo_endpoint ? 0 : -1;
}

void rpmsg_service_poll(uint32_t mailbox_pending)
{
    if (!rpmsg_instance)
        return;
    /*
     * Do not gate the virtqueue scan on a mailbox edge. A doorbell can be
     * missed: Linux orders its vring stores against the CPU2DSP write with a
     * fence, but the two land in different NoC destinations, so the interrupt
     * can be observed here before avail->idx is. Linux may also legitimately
     * suppress the kick via VRING_USED_F_NO_NOTIFY, which nothing initializes
     * on a fixed-DA carveout vring. A missed edge strands the message forever,
     * because there is no later notification for it. The main loop free-spins,
     * so an unconditional scan is effectively free.
     */
    if (!rsc_driver_ok())
        return;
#if K230_RPMSG_EDGE_GATED_POLL
    if (mailbox_pending)
        rpmsg_platform_poll();
#else
    (void)mailbox_pending;
    rpmsg_platform_poll();
#endif
    if (!announced && rpmsg_lite_is_link_up(rpmsg_instance)) {
        if (rpmsg_ns_announce(rpmsg_instance, echo_endpoint,
                              "rpmsg-raw", RL_NS_CREATE) == RL_SUCCESS)
            announced = 1;
    }
}

void rpmsg_service_publish_stats(void)
{
    volatile struct k230_rpmsg_stats *stats =
        (volatile struct k230_rpmsg_stats *)(uintptr_t)
        (AMP_SHM_PHYS_BASE + K230_RPMSG_STATS_OFFSET);
    struct virtqueue *rvq = rpmsg_instance ? rpmsg_instance->rvq : 0;
    struct virtqueue *tvq = rpmsg_instance ? rpmsg_instance->tvq : 0;

    stats->magic = K230_RPMSG_STATS_MAGIC;
    stats->link_up = (uint32_t)rpmsg_service_link_up();
    stats->reserved[0] = rpmsg_service_rsc_status();
    stats->reserved[1] = driver_ok;
    stats->reserved[2] = announced;
    stats->rx_callbacks = (uint32_t)rx_messages;
    stats->tx_sent = (uint32_t)tx_messages;
    stats->tx_failed = (uint32_t)tx_failures;
    stats->fetch_rx = (uint32_t)virtqueue_k230_fetch_count(1);
    stats->fetch_tx = (uint32_t)virtqueue_k230_fetch_count(0);
    if (rvq) {
        cache_invalidate_range((const void *)&rvq->vq_ring.avail->idx,
                               sizeof(rvq->vq_ring.avail->idx));
        stats->rvq_avail_idx = rvq->vq_ring.avail->idx;
        stats->rvq_consumed = rvq->vq_available_idx;
    }
    if (tvq) {
        cache_invalidate_range((const void *)&tvq->vq_ring.avail->idx,
                               sizeof(tvq->vq_ring.avail->idx));
        stats->tvq_avail_idx = tvq->vq_ring.avail->idx;
        stats->tvq_consumed = tvq->vq_available_idx;
    }
    cache_clean_range((const void *)stats, sizeof(*stats));
}

unsigned long rpmsg_service_rx_count(void) { return rx_messages; }
unsigned long rpmsg_service_tx_count(void) { return tx_messages; }
int rpmsg_service_link_up(void)
{
    return rpmsg_instance && rpmsg_lite_is_link_up(rpmsg_instance);
}

void rpmsg_service_print_debug(void)
{
    struct llist *head = rpmsg_instance ? rpmsg_instance->rl_endpoints : 0;
    uint32_t i;

    uart3_puts("RPMsg instance/ept/head/data: ");
    uart3_puthex64((uintptr_t)rpmsg_instance);
    uart3_puts("/");
    uart3_puthex64((uintptr_t)echo_endpoint);
    uart3_puts("/");
    uart3_puthex64((uintptr_t)head);
    uart3_puts("/");
    uart3_puthex64(head ? (uintptr_t)head->data : 0U);
    uart3_puts("\nRPMsg ept addr/last buffer: ");
    uart3_puthex64(echo_endpoint ? echo_endpoint->addr : 0U);
    uart3_puts("/");
    uart3_puthex64(rpmsg_platform_last_buffer());
    uart3_puts("\nRPMsg fetched rx/tx: ");
    uart3_puthex64(virtqueue_k230_fetch_count(1));
    uart3_puts("/");
    uart3_puthex64(virtqueue_k230_fetch_count(0));
    uart3_puts("\nRPMsg first rx desc idx:len:");
    for (i = 0; i < 4U; ++i) {
        uart3_puts(" ");
        uart3_puthex64(virtqueue_k230_first_desc_idx(i));
        uart3_puts(":");
        uart3_puthex64(virtqueue_k230_first_desc_len(i));
    }
    uart3_puts("\nRPMsg last buffer words:");
    for (i = 0; i < 4U; ++i) {
        uart3_puts(" ");
        uart3_puthex64(rpmsg_platform_last_buffer_word(i));
    }
    uart3_puts("\n");
}
