/*
 * HALTZ messages: IDs, tables and pack/unpack functions.
 *
 * Messages are never sent as C structs. Each pack function writes the payload byte by byte,
 * little-endian; each unpack function reads it back the same way.
 */
#ifndef HALTZ_MSGS_H
#define HALTZ_MSGS_H

#include <stddef.h>
#include <stdint.h>

#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HALTZ_MSG_HELLO = 0x01, ... */
typedef enum {
#define X(name, id, min_len, max_len) HALTZ_MSG_##name = (id),
#include "haltz/msg_ids.def"
#undef X
} haltz_msg_id_t;

/*
 * Payload sizes: HALTZ_MSG_x_MIN_LEN and HALTZ_MSG_x_MAX_LEN.
 * HALTZ_MSG_x_LEN = MAX_LEN: the buffer size a payload needs, and the exact size of a
 * fixed-size message (MIN = MAX), e.g. HALTZ_MSG_HELLO_LEN = 7.
 */
enum {
#define X(name, id, min_len, max_len)                                                          \
    HALTZ_MSG_##name##_MIN_LEN = (min_len), HALTZ_MSG_##name##_MAX_LEN = (max_len),           \
    HALTZ_MSG_##name##_LEN = (max_len),
#include "haltz/msg_ids.def"
#undef X
};

/* Number of message types in msg_ids.def. */
enum {
#define X(name, id, min_len, max_len) HALTZ_MSG_IDX_##name,
#include "haltz/msg_ids.def"
#undef X
    HALTZ_MSG_COUNT
};

/* Tables in msg_ids.def order, index 0..HALTZ_MSG_COUNT-1. */
extern const uint8_t haltz_msg_ids[HALTZ_MSG_COUNT];
extern const char *const haltz_msg_names[HALTZ_MSG_COUNT];
extern const uint8_t haltz_msg_min_lens[HALTZ_MSG_COUNT];
extern const uint8_t haltz_msg_max_lens[HALTZ_MSG_COUNT];

/* Lookups by wire ID. */
const char *haltz_msg_name(uint8_t id);   /* "HELLO", or "?" if unknown */
int haltz_msg_min_len(uint8_t id);        /* smallest allowed payload, or -1 if unknown */
int haltz_msg_max_len(uint8_t id);        /* largest allowed payload, or -1 if unknown */
/* OK, UNKNOWN_TYPE, or BAD_LEN if len is outside MIN_LEN..MAX_LEN. Finer length rules
 * (odd length, multiple of 8, ...) are checked by the message's unpack function. */
haltz_err_t haltz_msg_check(uint8_t id, size_t len);

/* ---- HELLO (0x01): first message each board sends after boot -------------------------- */
/* Payload: board_id u8 | proto_version u16 LE | git_hash 4 bytes (first 8 hex chars). */
typedef struct {
    uint8_t board_id;      /* haltz_board_id_t */
    uint16_t proto_version; /* HALTZ_PROTO_VERSION of the sender */
    uint8_t git_hash[4];   /* "1a2b3c4d" -> {0x1a, 0x2b, 0x3c, 0x4d} */
} haltz_hello_t;

haltz_err_t haltz_hello_pack(const haltz_hello_t *msg, uint8_t *buf, size_t cap);
haltz_err_t haltz_hello_unpack(haltz_hello_t *msg, const uint8_t *buf, size_t len);

/* ---- ACK (0x02) ------------------------------------------------------------------------ */
/* Payload: type being acknowledged u8. */
typedef struct {
    uint8_t type;
} haltz_ack_t;

haltz_err_t haltz_ack_pack(const haltz_ack_t *msg, uint8_t *buf, size_t cap);
haltz_err_t haltz_ack_unpack(haltz_ack_t *msg, const uint8_t *buf, size_t len);

/* ---- NAK (0x03) ------------------------------------------------------------------------ */
/* Payload: type being refused u8 | error code u8 (haltz_err_t). */
typedef struct {
    uint8_t type;
    uint8_t error; /* haltz_err_t */
} haltz_nak_t;

haltz_err_t haltz_nak_pack(const haltz_nak_t *msg, uint8_t *buf, size_t cap);
haltz_err_t haltz_nak_unpack(haltz_nak_t *msg, const uint8_t *buf, size_t len);

/* ---- git hash helpers ------------------------------------------------------------------ */
/* Parse the first 8 hex characters of hex (upper or lower case) into out[4].
 * Returns HALTZ_ERR_BAD_VALUE (and zeroes out) if they are not 8 hex digits. */
haltz_err_t haltz_git_hash_parse(const char *hex, uint8_t out[4]);
/* Write 8 lower-case hex chars plus NUL into out[9]. */
void haltz_git_hash_format(const uint8_t hash[4], char out[9]);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_MSGS_H */
