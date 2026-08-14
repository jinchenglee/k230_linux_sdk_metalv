#ifndef APRILTAG_BUFFER_TELEMETRY_H
#define APRILTAG_BUFFER_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

typedef struct apriltag_buffer_telemetry {
    uint64_t len;
    uint64_t capacity;
    uint64_t high_water;
    uint64_t growths_call;
    uint64_t growths_total;
    uint64_t capacity_bytes;
    uint64_t high_water_bytes;
    uint64_t element_size;
} apriltag_buffer_telemetry_t;

#ifdef __cplusplus
#define APRILTAG_BUFFER_TELEMETRY_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define APRILTAG_BUFFER_TELEMETRY_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif
#define APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(field, expected) \
    APRILTAG_BUFFER_TELEMETRY_STATIC_ASSERT(offsetof(apriltag_buffer_telemetry_t, field) == (expected), \
                                            "buffer telemetry offset mismatch: " #field)

APRILTAG_BUFFER_TELEMETRY_STATIC_ASSERT(sizeof(apriltag_buffer_telemetry_t) == 64,
                                        "buffer telemetry ABI size mismatch");
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(len, 0);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(capacity, 8);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(high_water, 16);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(growths_call, 24);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(growths_total, 32);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(capacity_bytes, 40);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(high_water_bytes, 48);
APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET(element_size, 56);

#undef APRILTAG_BUFFER_TELEMETRY_ASSERT_OFFSET
#undef APRILTAG_BUFFER_TELEMETRY_STATIC_ASSERT

#endif
