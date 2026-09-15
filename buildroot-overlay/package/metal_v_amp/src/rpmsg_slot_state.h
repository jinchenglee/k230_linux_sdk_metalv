#ifndef METAL_V_RPMSG_SLOT_STATE_H
#define METAL_V_RPMSG_SLOT_STATE_H

#include <stdint.h>

#include "rpmsg_protocol.h"

struct k230_slot_ownership {
    uint32_t busy_mask;
    uint32_t depth;
    uint32_t high_water;
};

static inline uint16_t
k230_slot_claim(struct k230_slot_ownership *state, uint32_t slot_id)
{
    uint32_t bit;

    if (slot_id >= K230_PAYLOAD_SLOT_COUNT)
        return K230_RPMSG_STATUS_INVALID_SLOT;
    bit = UINT32_C(1) << slot_id;
    if (state->busy_mask & bit)
        return K230_RPMSG_STATUS_SLOT_BUSY;
    if (state->depth >= K230_PAYLOAD_SLOT_COUNT)
        return K230_RPMSG_STATUS_QUEUE_FULL;
    state->busy_mask |= bit;
    ++state->depth;
    if (state->depth > state->high_water)
        state->high_water = state->depth;
    return K230_RPMSG_STATUS_OK;
}

static inline uint32_t
k230_slot_release(struct k230_slot_ownership *state, uint32_t slot_id)
{
    uint32_t bit;

    if (slot_id >= K230_PAYLOAD_SLOT_COUNT)
        return 0;
    bit = UINT32_C(1) << slot_id;
    if (!(state->busy_mask & bit) || !state->depth)
        return 0;
    state->busy_mask &= ~bit;
    --state->depth;
    return 1;
}

static inline uint32_t k230_slot_reset(struct k230_slot_ownership *state)
{
    uint32_t dropped = state->depth;

    state->busy_mask = 0;
    state->depth = 0;
    return dropped;
}

#endif
