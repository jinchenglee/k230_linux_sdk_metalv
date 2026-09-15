#include <stdint.h>
#include <string.h>

#include "cache.h"
#include "rpmsg_config.h"
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_platform.h"
#include "rpmsg_protocol.h"
#include "rpmsg_slot_state.h"
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
static uint64_t protocol_generation;
static unsigned long protocol_hellos;
static unsigned long protocol_messages;
static unsigned long rejected_version;
static unsigned long rejected_capabilities;
static unsigned long rejected_generation;
static unsigned long endpoint_restarts;
static unsigned long endpoint_restart_failures;
static uint32_t endpoint_restart_pending;
static unsigned long slot_submitted;
static unsigned long slot_completed;
static unsigned long slot_rejected;
static unsigned long slot_crc_mismatch;
static unsigned long slot_dropped_restart;
static uint32_t slot_queue_head;
static uint32_t slot_queue_tail;
static struct k230_slot_ownership slot_ownership;

struct k230_slot_job {
    struct k230_rpmsg_slot_submit request;
    struct k230_rpmsg_slot_complete response;
    uint32_t src;
    uint32_t processed;
};

static struct k230_slot_job slot_queue[K230_PAYLOAD_SLOT_COUNT];

static int32_t echo_rx(void *payload, uint32_t payload_len, uint32_t src,
                       void *priv);

static inline uint64_t read_cycle(void)
{
    uint64_t value;

    __asm__ volatile ("rdcycle %0" : "=r"(value));
    return value;
}

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

static void protocol_advance_generation(void)
{
    volatile struct k230_rpmsg_stats *stats =
        (volatile struct k230_rpmsg_stats *)(uintptr_t)
        (AMP_SHM_PHYS_BASE + K230_RPMSG_STATS_OFFSET);

    if (!protocol_generation) {
        cache_invalidate_range((const void *)stats, sizeof(*stats));
        if (stats->magic == K230_RPMSG_STATS_MAGIC)
            protocol_generation =
                ((uint64_t)stats->generation_hi << 32) |
                stats->generation_lo;
    }
    ++protocol_generation;
    if (!protocol_generation)
        protocol_generation = 1;
}

static int32_t send_message(uint32_t dst, void *payload,
                            uint32_t payload_len)
{
    int32_t ret = rpmsg_lite_send(rpmsg_instance, echo_endpoint, dst, payload,
                                  payload_len, RL_DONT_BLOCK);

    if (ret == RL_SUCCESS)
        ++tx_messages;
    else
        ++tx_failures;
    return ret;
}

static void protocol_reply_header(struct k230_rpmsg_protocol_header *reply,
                                  const struct k230_rpmsg_protocol_header *request,
                                  uint16_t type, uint16_t status,
                                  uint32_t message_size)
{
    memset(reply, 0, sizeof(*reply));
    reply->magic = K230_RPMSG_PROTOCOL_MAGIC;
    reply->version = K230_RPMSG_PROTOCOL_VERSION;
    reply->header_size = K230_RPMSG_PROTOCOL_HEADER_SIZE;
    reply->type = type;
    reply->status = status;
    reply->message_size = message_size;
    reply->generation = protocol_generation;
    reply->sequence = request ? request->sequence : 0;
    reply->capabilities = K230_RPMSG_CAPABILITIES;
}

static int32_t send_protocol_error(
    uint32_t dst, const struct k230_rpmsg_protocol_header *request,
    uint16_t status)
{
    struct k230_rpmsg_protocol_header reply;

    protocol_reply_header(&reply, request, K230_RPMSG_MSG_ERROR, status,
                          sizeof(reply));
    (void)send_message(dst, &reply, sizeof(reply));
    return RL_RELEASE;
}

static int32_t enqueue_slot_request(void *payload, uint32_t payload_len,
                                    uint32_t src,
                                    const struct k230_rpmsg_protocol_header *header)
{
    struct k230_rpmsg_slot_submit request;
    struct k230_slot_job *job;
    uint16_t status;

    if (header->generation != protocol_generation) {
        ++rejected_generation;
        ++slot_rejected;
        return send_protocol_error(src, header,
                                   K230_RPMSG_STATUS_STALE_GENERATION);
    }
    if (payload_len != sizeof(request)) {
        ++slot_rejected;
        return send_protocol_error(src, header,
                                   K230_RPMSG_STATUS_BAD_SIZE);
    }
    memcpy(&request, payload, sizeof(request));
    status = k230_rpmsg_validate_slot(&request);
    if (status != K230_RPMSG_STATUS_OK) {
        ++slot_rejected;
        return send_protocol_error(src, header, status);
    }
    status = k230_slot_claim(&slot_ownership, request.slot_id);
    if (status != K230_RPMSG_STATUS_OK) {
        ++slot_rejected;
        return send_protocol_error(src, header, status);
    }

    job = &slot_queue[slot_queue_tail];
    memset(job, 0, sizeof(*job));
    memcpy(&job->request, &request, sizeof(request));
    job->src = src;
    slot_queue_tail = (slot_queue_tail + 1U) % K230_PAYLOAD_SLOT_COUNT;
    ++slot_submitted;
    return RL_RELEASE;
}

static void reset_slot_queue(void)
{
    slot_dropped_restart += k230_slot_reset(&slot_ownership);
    slot_queue_head = 0;
    slot_queue_tail = 0;
}

static void process_slot_queue(void)
{
    struct k230_slot_job *job;
    const uint8_t *payload;
    uint64_t started;
    uint16_t status;

    if (!slot_ownership.depth || !echo_endpoint)
        return;
    job = &slot_queue[slot_queue_head];
    if (!job->processed) {
        payload = (const uint8_t *)(uintptr_t)
            (K230_PAYLOAD_POOL_BASE + job->request.offset);
        protocol_reply_header(&job->response.header, &job->request.header,
                              K230_RPMSG_MSG_SLOT_COMPLETE,
                              K230_RPMSG_STATUS_OK,
                              sizeof(job->response));
        job->response.slot_id = job->request.slot_id;
        job->response.flags = job->request.flags;
        job->response.offset = job->request.offset;
        job->response.data_length = job->request.data_length;

        started = read_cycle();
        cache_invalidate_range(payload, job->request.padded_length);
        amp_acquire_fence();
        job->response.invalidate_cycles = read_cycle() - started;
        started = read_cycle();
        job->response.observed_crc =
            amp_crc32(payload, job->request.data_length);
        job->response.crc_cycles = read_cycle() - started;
        status = job->response.observed_crc == job->request.expected_crc ?
            K230_RPMSG_STATUS_OK : K230_RPMSG_STATUS_CRC_MISMATCH;
        job->response.header.status = status;
        if (status == K230_RPMSG_STATUS_CRC_MISMATCH)
            ++slot_crc_mismatch;
        job->processed = 1;
    }
    if (send_message(job->src, &job->response,
                     sizeof(job->response)) != RL_SUCCESS)
        return;
    (void)k230_slot_release(&slot_ownership, job->request.slot_id);
    slot_queue_head = (slot_queue_head + 1U) % K230_PAYLOAD_SLOT_COUNT;
    ++slot_completed;
}

static int32_t protocol_rx(void *payload, uint32_t payload_len, uint32_t src)
{
    uint8_t reply_buffer[RL_BUFFER_PAYLOAD_SIZE]
        __attribute__((aligned(sizeof(uint64_t))));
    struct k230_rpmsg_protocol_header request;
    struct k230_rpmsg_protocol_header reply;
    uint32_t reply_len;

    ++protocol_messages;
    if (payload_len < sizeof(request))
        return send_protocol_error(src, 0, K230_RPMSG_STATUS_BAD_HEADER);

    memcpy(&request, payload, sizeof(request));
    if (request.version != K230_RPMSG_PROTOCOL_VERSION) {
        ++rejected_version;
        return send_protocol_error(src, &request,
                                   K230_RPMSG_STATUS_BAD_VERSION);
    }
    if (request.header_size != sizeof(request))
        return send_protocol_error(src, &request,
                                   K230_RPMSG_STATUS_BAD_HEADER);
    if (request.message_size != payload_len ||
        request.message_size > RL_BUFFER_PAYLOAD_SIZE)
        return send_protocol_error(src, &request,
                                   K230_RPMSG_STATUS_BAD_SIZE);

    switch (request.type) {
    case K230_RPMSG_MSG_HELLO:
        ++protocol_hellos;
        if (request.capabilities & ~K230_RPMSG_CAPABILITIES) {
            ++rejected_capabilities;
            return send_protocol_error(
                src, &request,
                K230_RPMSG_STATUS_UNSUPPORTED_CAPABILITY);
        }
        if (payload_len != sizeof(request))
            return send_protocol_error(src, &request,
                                       K230_RPMSG_STATUS_BAD_SIZE);
        protocol_reply_header(&reply, &request,
                              K230_RPMSG_MSG_HELLO_REPLY,
                              K230_RPMSG_STATUS_OK, sizeof(reply));
        (void)send_message(src, &reply, sizeof(reply));
        return RL_RELEASE;

    case K230_RPMSG_MSG_ECHO:
        if (request.generation != protocol_generation) {
            ++rejected_generation;
            return send_protocol_error(src, &request,
                                       K230_RPMSG_STATUS_STALE_GENERATION);
        }
        reply_len = payload_len;
        memcpy(reply_buffer, payload, payload_len);
        protocol_reply_header(&reply, &request,
                              K230_RPMSG_MSG_ECHO_REPLY,
                              K230_RPMSG_STATUS_OK, reply_len);
        memcpy(reply_buffer, &reply, sizeof(reply));
        (void)send_message(src, reply_buffer, reply_len);
        return RL_RELEASE;

    case K230_RPMSG_MSG_RESTART_ENDPOINT:
        if (request.generation != protocol_generation) {
            ++rejected_generation;
            return send_protocol_error(src, &request,
                                       K230_RPMSG_STATUS_STALE_GENERATION);
        }
        if (payload_len != sizeof(request))
            return send_protocol_error(src, &request,
                                       K230_RPMSG_STATUS_BAD_SIZE);
        protocol_reply_header(&reply, &request,
                              K230_RPMSG_MSG_RESTART_ENDPOINT_REPLY,
                              K230_RPMSG_STATUS_OK, sizeof(reply));
        if (send_message(src, &reply, sizeof(reply)) == RL_SUCCESS)
            endpoint_restart_pending = 1;
        return RL_RELEASE;

    case K230_RPMSG_MSG_SLOT_SUBMIT:
        return enqueue_slot_request(payload, payload_len, src, &request);

    default:
        return send_protocol_error(src, &request,
                                   K230_RPMSG_STATUS_BAD_TYPE);
    }
}

static int32_t echo_rx(void *payload, uint32_t payload_len, uint32_t src,
                       void *priv)
{
    uint32_t magic = 0;

    (void)priv;
    ++rx_messages;
    if (payload_len >= sizeof(magic))
        memcpy(&magic, payload, sizeof(magic));
    if (magic == K230_RPMSG_PROTOCOL_MAGIC)
        return protocol_rx(payload, payload_len, src);
    (void)send_message(src, payload, payload_len);
    return RL_RELEASE;
}

static void restart_endpoint_if_requested(void)
{
    if (!endpoint_restart_pending)
        return;
    endpoint_restart_pending = 0;
    reset_slot_queue();
    if (!echo_endpoint ||
        rpmsg_lite_destroy_ept(rpmsg_instance, echo_endpoint) != RL_SUCCESS) {
        ++endpoint_restart_failures;
        return;
    }
    echo_endpoint = 0;
    echo_endpoint = rpmsg_lite_create_ept(
        rpmsg_instance, K230_RPMSG_ENDPOINT, echo_rx, 0,
        &echo_endpoint_context);
    if (!echo_endpoint) {
        ++endpoint_restart_failures;
        return;
    }
    protocol_advance_generation();
    ++endpoint_restarts;
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
    protocol_advance_generation();
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
    restart_endpoint_if_requested();
    process_slot_queue();
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
    stats->rsc_status = rpmsg_service_rsc_status();
    stats->driver_ok = driver_ok;
    stats->announced = announced;
    stats->generation_lo = (uint32_t)protocol_generation;
    stats->generation_hi = (uint32_t)(protocol_generation >> 32);
    stats->hellos = (uint32_t)protocol_hellos;
    stats->protocol_messages = (uint32_t)protocol_messages;
    stats->rejected_version = (uint32_t)rejected_version;
    stats->rejected_capabilities = (uint32_t)rejected_capabilities;
    stats->rejected_generation = (uint32_t)rejected_generation;
    stats->endpoint_restarts = (uint32_t)endpoint_restarts;
    stats->endpoint_restart_failures =
        (uint32_t)endpoint_restart_failures;
    stats->slot_submitted = (uint32_t)slot_submitted;
    stats->slot_completed = (uint32_t)slot_completed;
    stats->slot_rejected = (uint32_t)slot_rejected;
    stats->slot_crc_mismatch = (uint32_t)slot_crc_mismatch;
    stats->slot_dropped_restart = (uint32_t)slot_dropped_restart;
    stats->slot_busy_mask = slot_ownership.busy_mask;
    stats->slot_queue_depth = slot_ownership.depth;
    stats->slot_queue_high_water = slot_ownership.high_water;
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
