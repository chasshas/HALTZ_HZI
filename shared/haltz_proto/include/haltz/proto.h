/*
 * HALTZ inter-board protocol: global constants and error codes.
 *
 * Frame on the wire:
 *   SYNC (0xA5) | TYPE (u8) | LEN (u8, 0..255) | PAYLOAD (LEN bytes) | CRC8
 * CRC-8 covers TYPE, LEN and PAYLOAD (see crc8.h).
 * All multi-byte fields are little-endian. Enums travel as uint8_t.
 */
#ifndef HALTZ_PROTO_H
#define HALTZ_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the frame format or an existing message's layout changes. */
#define HALTZ_PROTO_VERSION 1u

#define HALTZ_SYNC           0xA5u
#define HALTZ_MAX_PAYLOAD    255u /* LEN is a u8 */
#define HALTZ_FRAME_OVERHEAD 4u   /* SYNC + TYPE + LEN + CRC8 */
#define HALTZ_MAX_FRAME      (HALTZ_MAX_PAYLOAD + HALTZ_FRAME_OVERHEAD)

/*
 * Error codes. Returned by library functions and sent in NAK payloads (as u8),
 * so the numeric values are part of the protocol: never renumber, only append.
 */
typedef enum {
    HALTZ_OK = 0,
    HALTZ_ERR_ARG = 1,          /* bad argument (NULL pointer, ...) */
    HALTZ_ERR_NO_SPACE = 2,     /* output buffer too small */
    HALTZ_ERR_BAD_CRC = 3,      /* frame CRC mismatch */
    HALTZ_ERR_BAD_LEN = 4,      /* payload length wrong for this message type */
    HALTZ_ERR_UNKNOWN_TYPE = 5, /* message TYPE not in msg_ids.def */
    HALTZ_ERR_VERSION = 6,      /* peer speaks another HALTZ_PROTO_VERSION */
    HALTZ_ERR_BUSY = 7,         /* receiver cannot take the message right now */
    HALTZ_ERR_BAD_VALUE = 8,    /* a field holds a value out of range */
} haltz_err_t;

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_PROTO_H */
