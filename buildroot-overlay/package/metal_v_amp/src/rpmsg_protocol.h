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
#define K230_RPMSG_CAPABILITIES \
	(K230_RPMSG_CAP_ECHO | K230_RPMSG_CAP_GENERATION | \
	 K230_RPMSG_CAP_ENDPOINT_RESTART)
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

#endif
