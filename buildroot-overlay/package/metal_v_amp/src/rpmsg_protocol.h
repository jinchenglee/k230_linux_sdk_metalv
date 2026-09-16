#ifndef METAL_V_RPMSG_PROTOCOL_H
#define METAL_V_RPMSG_PROTOCOL_H

#include <stdint.h>

/*
 * Versioned control protocol carried inside an RPMsg payload.  Both peers are
 * little-endian K230 cores, so fields are transferred in native byte order.
 * Image data never belongs in this structure: future requests carry shared
 * payload-slot descriptors after this header.
 */
#define K230_RPMSG_PROTOCOL_MAGIC   UINT32_C(0x4b32414d) /* "K2AM" */
#define K230_RPMSG_PROTOCOL_VERSION UINT16_C(1)

#define K230_RPMSG_CAP_ECHO             (UINT64_C(1) << 0)
#define K230_RPMSG_CAP_GENERATION       (UINT64_C(1) << 1)
#define K230_RPMSG_CAP_ENDPOINT_RESTART (UINT64_C(1) << 2)
#define K230_RPMSG_CAP_PAYLOAD_SLOTS    (UINT64_C(1) << 3)
#define K230_RPMSG_CAP_CAMERA_BUFFERS   (UINT64_C(1) << 4)
#define K230_RPMSG_CAPABILITIES \
	(K230_RPMSG_CAP_ECHO | K230_RPMSG_CAP_GENERATION | \
	 K230_RPMSG_CAP_ENDPOINT_RESTART | K230_RPMSG_CAP_PAYLOAD_SLOTS | \
	 K230_RPMSG_CAP_CAMERA_BUFFERS)
#define K230_RPMSG_REQUIRED_CAPABILITIES \
	(K230_RPMSG_CAP_ECHO | K230_RPMSG_CAP_GENERATION)

enum k230_rpmsg_message_type {
	K230_RPMSG_MSG_HELLO = 1,
	K230_RPMSG_MSG_HELLO_REPLY = 2,
	K230_RPMSG_MSG_ECHO = 3,
	K230_RPMSG_MSG_ECHO_REPLY = 4,
	K230_RPMSG_MSG_RESTART_ENDPOINT = 5,
	K230_RPMSG_MSG_RESTART_ENDPOINT_REPLY = 6,
	K230_RPMSG_MSG_ERROR = 7,
	K230_RPMSG_MSG_SLOT_SUBMIT = 8,
	K230_RPMSG_MSG_SLOT_COMPLETE = 9,
	K230_RPMSG_MSG_CAMERA_REGISTER = 10,
	K230_RPMSG_MSG_CAMERA_REGISTER_REPLY = 11,
	K230_RPMSG_MSG_CAMERA_SUBMIT = 12,
	K230_RPMSG_MSG_CAMERA_COMPLETE = 13,
};

enum k230_rpmsg_status {
	K230_RPMSG_STATUS_OK = 0,
	K230_RPMSG_STATUS_BAD_VERSION = 1,
	K230_RPMSG_STATUS_BAD_HEADER = 2,
	K230_RPMSG_STATUS_BAD_SIZE = 3,
	K230_RPMSG_STATUS_UNSUPPORTED_CAPABILITY = 4,
	K230_RPMSG_STATUS_STALE_GENERATION = 5,
	K230_RPMSG_STATUS_BAD_TYPE = 6,
	K230_RPMSG_STATUS_RESTART_FAILED = 7,
	K230_RPMSG_STATUS_INVALID_SLOT = 8,
	K230_RPMSG_STATUS_INVALID_RANGE = 9,
	K230_RPMSG_STATUS_SLOT_BUSY = 10,
	K230_RPMSG_STATUS_QUEUE_FULL = 11,
	K230_RPMSG_STATUS_CRC_MISMATCH = 12,
	K230_RPMSG_STATUS_BAD_FORMAT = 13,
	K230_RPMSG_STATUS_NOT_REGISTERED = 14,
	K230_RPMSG_STATUS_ALREADY_REGISTERED = 15,
};

struct k230_rpmsg_protocol_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint16_t type;
	uint16_t status;
	uint32_t message_size;
	uint64_t generation;
	uint64_t sequence;
	uint64_t capabilities;
};

#define K230_RPMSG_PROTOCOL_HEADER_SIZE UINT16_C(40)

_Static_assert(sizeof(struct k230_rpmsg_protocol_header) ==
	       K230_RPMSG_PROTOCOL_HEADER_SIZE,
	       "K230 RPMsg protocol header layout changed");

/*
 * Linux owns a slot while filling it. SLOT_SUBMIT transfers ownership to the
 * big core; SLOT_COMPLETE returns it to Linux. Offsets are relative to this
 * pool, never arbitrary physical addresses. The padded extent is the cache
 * maintenance boundary while data_length is the CRC-covered payload.
 */
#define K230_PAYLOAD_POOL_BASE       UINT64_C(0x1d600000)
#define K230_PAYLOAD_SLOT_SIZE       UINT32_C(0x00100000)
#define K230_PAYLOAD_SLOT_COUNT      UINT32_C(4)
#define K230_PAYLOAD_POOL_SIZE \
	(K230_PAYLOAD_SLOT_SIZE * K230_PAYLOAD_SLOT_COUNT)
#define K230_PAYLOAD_CACHE_LINE      UINT32_C(64)
#define K230_AMP_RESERVED_END        UINT64_C(0x20000000)

enum k230_payload_format {
	K230_PAYLOAD_FORMAT_OPAQUE = 0,
	K230_PAYLOAD_FORMAT_Y8 = 1,
};

struct k230_rpmsg_slot_submit {
	struct k230_rpmsg_protocol_header header;
	uint32_t slot_id;
	uint32_t flags;
	uint64_t offset;
	uint32_t data_length;
	uint32_t padded_length;
	uint32_t expected_crc;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t reserved;
};

struct k230_rpmsg_slot_complete {
	struct k230_rpmsg_protocol_header header;
	uint32_t slot_id;
	uint32_t flags;
	uint64_t offset;
	uint32_t data_length;
	uint32_t observed_crc;
	uint64_t invalidate_cycles;
	uint64_t crc_cycles;
};

static inline uint16_t
k230_rpmsg_validate_slot(const struct k230_rpmsg_slot_submit *request)
{
	uint64_t expected_offset;
	uint64_t image_bytes;

	if (request->slot_id >= K230_PAYLOAD_SLOT_COUNT)
		return K230_RPMSG_STATUS_INVALID_SLOT;
	expected_offset = (uint64_t)request->slot_id * K230_PAYLOAD_SLOT_SIZE;
	if (request->offset != expected_offset ||
	    (request->offset % K230_PAYLOAD_CACHE_LINE) != 0 ||
	    !request->data_length ||
	    request->data_length > request->padded_length ||
	    request->padded_length > K230_PAYLOAD_SLOT_SIZE ||
	    (request->padded_length % K230_PAYLOAD_CACHE_LINE) != 0)
		return K230_RPMSG_STATUS_INVALID_RANGE;
	if (request->format == K230_PAYLOAD_FORMAT_OPAQUE)
		return K230_RPMSG_STATUS_OK;
	if (request->format != K230_PAYLOAD_FORMAT_Y8 ||
	    !request->width || !request->height ||
	    request->stride < request->width)
		return K230_RPMSG_STATUS_BAD_FORMAT;
	image_bytes = (uint64_t)request->stride * request->height;
	if (image_bytes != request->data_length)
		return K230_RPMSG_STATUS_BAD_FORMAT;
	return K230_RPMSG_STATUS_OK;
}

#define K230_RPMSG_SLOT_SUBMIT_SIZE   UINT32_C(88)
#define K230_RPMSG_SLOT_COMPLETE_SIZE UINT32_C(80)

_Static_assert(sizeof(struct k230_rpmsg_slot_submit) ==
	       K230_RPMSG_SLOT_SUBMIT_SIZE,
	       "K230 RPMsg slot-submit layout changed");
_Static_assert(sizeof(struct k230_rpmsg_slot_complete) ==
	       K230_RPMSG_SLOT_COMPLETE_SIZE,
	       "K230 RPMsg slot-complete layout changed");
_Static_assert((K230_PAYLOAD_POOL_BASE % K230_PAYLOAD_CACHE_LINE) == 0,
	       "payload pool must be cache-line aligned");
_Static_assert((K230_PAYLOAD_SLOT_SIZE % K230_PAYLOAD_CACHE_LINE) == 0,
	       "payload slots must be cache-line aligned");
_Static_assert(K230_PAYLOAD_POOL_BASE + K230_PAYLOAD_POOL_SIZE <=
	       K230_AMP_RESERVED_END,
	       "payload pool exceeds the reserved AMP region");

/* Registration is generation-scoped. remote_token is opaque to the protocol;
 * the remote platform backend validates and resolves it once at registration.
 * Per-frame submission uses only the registered ID. */
#define AMP_RPMSG_CAMERA_BUFFER_MAX UINT32_C(16)

struct k230_rpmsg_camera_register {
	struct k230_rpmsg_protocol_header header;
	uint32_t buffer_id;
	uint32_t flags;
	uint64_t remote_token;
	uint64_t capacity;
};

struct k230_rpmsg_camera_submit {
	struct k230_rpmsg_protocol_header header;
	uint32_t buffer_id;
	uint32_t flags;
	uint32_t data_length;
	uint32_t padded_length;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

struct k230_rpmsg_camera_complete {
	struct k230_rpmsg_protocol_header header;
	uint32_t buffer_id;
	uint32_t flags;
	uint32_t data_length;
	uint32_t observed_crc;
	uint64_t invalidate_cycles;
	uint64_t crc_cycles;
};

static inline uint16_t
k230_rpmsg_validate_camera_registration(
	const struct k230_rpmsg_camera_register *request)
{
	if (request->buffer_id >= AMP_RPMSG_CAMERA_BUFFER_MAX)
		return K230_RPMSG_STATUS_INVALID_SLOT;
	if (request->flags || !request->capacity ||
	    (request->remote_token % K230_PAYLOAD_CACHE_LINE) != 0 ||
	    request->capacity < K230_PAYLOAD_CACHE_LINE)
		return K230_RPMSG_STATUS_INVALID_RANGE;
	return K230_RPMSG_STATUS_OK;
}

static inline uint16_t
k230_rpmsg_validate_camera_submit(
	const struct k230_rpmsg_camera_submit *request)
{
	uint64_t image_bytes;

	if (request->buffer_id >= AMP_RPMSG_CAMERA_BUFFER_MAX)
		return K230_RPMSG_STATUS_INVALID_SLOT;
	if (request->flags || !request->data_length ||
	    request->data_length > request->padded_length ||
	    (request->padded_length % K230_PAYLOAD_CACHE_LINE) != 0)
		return K230_RPMSG_STATUS_INVALID_RANGE;
	if (request->format != K230_PAYLOAD_FORMAT_Y8 ||
	    !request->width || !request->height ||
	    request->stride < request->width)
		return K230_RPMSG_STATUS_BAD_FORMAT;
	image_bytes = (uint64_t)request->stride * request->height;
	if (image_bytes != request->data_length)
		return K230_RPMSG_STATUS_BAD_FORMAT;
	return K230_RPMSG_STATUS_OK;
}

#define K230_RPMSG_CAMERA_REGISTER_SIZE UINT32_C(64)
#define K230_RPMSG_CAMERA_SUBMIT_SIZE   UINT32_C(72)
#define K230_RPMSG_CAMERA_COMPLETE_SIZE UINT32_C(72)

_Static_assert(sizeof(struct k230_rpmsg_camera_register) ==
	       K230_RPMSG_CAMERA_REGISTER_SIZE,
	       "K230 RPMsg camera-register layout changed");
_Static_assert(sizeof(struct k230_rpmsg_camera_submit) ==
	       K230_RPMSG_CAMERA_SUBMIT_SIZE,
	       "K230 RPMsg camera-submit layout changed");
_Static_assert(sizeof(struct k230_rpmsg_camera_complete) ==
	       K230_RPMSG_CAMERA_COMPLETE_SIZE,
	       "K230 RPMsg camera-complete layout changed");
#endif
