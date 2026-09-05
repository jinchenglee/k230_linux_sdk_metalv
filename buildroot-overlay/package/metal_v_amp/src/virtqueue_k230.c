#include <stdint.h>

#include "rpmsg_env.h"
#include "rpmsg_platform.h"
#include "virtqueue.h"

/*
 * Upstream RPMsg-Lite invalidates the avail ring but assumes that descriptors
 * are coherent or immutable. Linux updates a TX descriptor's length before
 * publishing it. On the non-coherent K230 AMP path, invalidate and reread the
 * selected descriptor before its address and length are consumed.
 */
void *virtqueue_get_available_buffer_unfixed(struct virtqueue *vq,
                                              uint16_t *avail_idx,
                                              uint32_t *len);

static unsigned long fetch_count[2];
static uint32_t first_desc_len[8];
static uint32_t first_desc_idx[8];
static uint32_t first_desc_seen;

void *virtqueue_get_available_buffer(struct virtqueue *vq, uint16_t *avail_idx,
                                     uint32_t *len)
{
    struct vring_desc *desc;
    uint16_t head_idx;
    void *buffer;

    /* Invalidate avail->idx before it is read. */
    VQUEUE_INVALIDATE(&vq->vq_ring.avail->idx, sizeof(vq->vq_ring.avail->idx));
    if (vq->vq_available_idx == vq->vq_ring.avail->idx)
        return VQ_NULL;

    head_idx = (uint16_t)(vq->vq_available_idx &
                          (uint16_t)(vq->vq_nentries - 1U));
    VQUEUE_INVALIDATE(&vq->vq_ring.avail->ring[head_idx],
                      sizeof(vq->vq_ring.avail->ring[head_idx]));
    *avail_idx = vq->vq_ring.avail->ring[head_idx];
    env_rmb();

    /*
     * Invalidate and reread the descriptor BEFORE its address is used. Upstream
     * derives the buffer pointer from a possibly stale descriptor and only then
     * lets a wrapper re-read it, which is too late: on the first pass over the
     * ring this core still holds the zeroed descriptor lines it cached before
     * Linux populated them, so the stale addr reads as 0, env_map_patova()
     * yields NULL, and the caller treats a real message as "no buffer" after
     * vq_available_idx has already been advanced -- the message is consumed and
     * silently dropped. Only advance the index once the buffer is truly taken.
     */
    desc = &vq->vq_ring.desc[*avail_idx];
    VQUEUE_INVALIDATE(desc, sizeof(*desc));
    env_rmb();
    *len = desc->len;
#if defined(RL_USE_ENVIRONMENT_CONTEXT) && (RL_USE_ENVIRONMENT_CONTEXT == 1)
    buffer = env_map_patova(vq->env, (uint32_t)desc->addr);
#else
    buffer = env_map_patova((uint32_t)desc->addr);
#endif
    vq->vq_available_idx++;

    fetch_count[vq->vq_queue_index & 1U]++;
    if ((vq->vq_queue_index & 1U) == 1U && first_desc_seen < 8U) {
        first_desc_len[first_desc_seen] = *len;
        first_desc_idx[first_desc_seen] = *avail_idx;
        ++first_desc_seen;
    }
    return buffer;
}

unsigned long virtqueue_k230_fetch_count(uint32_t queue)
{
    return fetch_count[queue & 1U];
}

uint32_t virtqueue_k230_first_desc_len(uint32_t i)
{
    return i < 8U ? first_desc_len[i] : 0U;
}

uint32_t virtqueue_k230_first_desc_idx(uint32_t i)
{
    return i < 8U ? first_desc_idx[i] : 0U;
}
