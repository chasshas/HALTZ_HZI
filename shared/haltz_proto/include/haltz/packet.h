/*
 * HALTZ frame encoder and byte-at-a-time decoder.
 *
 *   SYNC (0xA5) | TYPE (u8) | LEN (u8) | PAYLOAD (LEN bytes) | CRC8 over TYPE..PAYLOAD
 *
 * The decoder is transport-agnostic (UART, SPI, test buffers) and never allocates.
 */
#ifndef HALTZ_PACKET_H
#define HALTZ_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A decoded frame. payload[0..len) is valid. */
typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[HALTZ_MAX_PAYLOAD];
} haltz_frame_t;

/*
 * Write one frame into out[0..out_cap). payload may be NULL when len is 0.
 * On success *out_len (if not NULL) = len + HALTZ_FRAME_OVERHEAD.
 * Errors: HALTZ_ERR_ARG, HALTZ_ERR_BAD_LEN (len > 255), HALTZ_ERR_NO_SPACE.
 */
haltz_err_t haltz_frame_encode(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out,
                               size_t out_cap, size_t *out_len);

typedef enum {
    HALTZ_DEC_NEED_MORE = 0, /* nothing complete yet, feed more bytes */
    HALTZ_DEC_FRAME = 1,     /* a valid frame is ready: haltz_decoder_frame() */
    HALTZ_DEC_CRC_ERROR = 2, /* a candidate frame failed its CRC and was dropped */
} haltz_dec_result_t;

/*
 * Decoder state. Treat as opaque; it is public only so callers can allocate it statically.
 *
 * Resync: after a bad CRC the decoder does not throw away everything it has seen.
 * It replays the bytes that followed the false SYNC, so a real frame that started
 * inside the rejected one is still found.
 */
typedef struct {
    uint8_t state;
    uint8_t crc;
    uint16_t raw_len;                /* bytes of the current candidate frame */
    uint16_t backlog_head;           /* replay queue: backlog[head .. head+len) */
    uint16_t backlog_len;
    uint8_t raw[HALTZ_MAX_FRAME];     /* SYNC TYPE LEN PAYLOAD... of the current candidate */
    uint8_t backlog[HALTZ_MAX_FRAME]; /* bytes waiting to be (re)processed after a resync */
    haltz_frame_t frame;             /* last good frame */
    uint32_t frames_ok;              /* statistics, wrap around */
    uint32_t crc_errors;
    uint32_t flushes;       /* partial frames given up by haltz_decoder_flush() */
    uint32_t bytes_skipped; /* bytes dropped while hunting for SYNC */
} haltz_decoder_t;

void haltz_decoder_init(haltz_decoder_t *dec);

/*
 * Feed one received byte. Returns as soon as an event happens.
 *
 * After a resync the decoder may still hold bytes that can contain more frames.
 * Whenever feed (or poll) returns something other than HALTZ_DEC_NEED_MORE, call
 * haltz_decoder_poll() until it returns HALTZ_DEC_NEED_MORE:
 *
 *     for (haltz_dec_result_t r = haltz_decoder_feed(&d, b); r != HALTZ_DEC_NEED_MORE;
 *          r = haltz_decoder_poll(&d)) {
 *         if (r == HALTZ_DEC_FRAME) handle(haltz_decoder_frame(&d));
 *     }
 *
 * (If you forget to poll nothing is lost, the held bytes are processed on the next feed.)
 */
haltz_dec_result_t haltz_decoder_feed(haltz_decoder_t *dec, uint8_t byte);

/* Process held-back bytes without new input. Returns HALTZ_DEC_NEED_MORE when there are none. */
haltz_dec_result_t haltz_decoder_poll(haltz_decoder_t *dec);

/*
 * Call when the line has been idle for a while (e.g. no byte for a few byte-times) while a
 * frame was in progress. A false SYNC in line noise can announce a LEN that never arrives
 * and would hold back real frames behind it; flush gives up on the partial candidate and
 * rescans its bytes. Returns like poll (handle events, then poll until NEED_MORE).
 * Does nothing when no frame is in progress.
 */
haltz_dec_result_t haltz_decoder_flush(haltz_decoder_t *dec);

/* The frame reported by the last HALTZ_DEC_FRAME. Valid until the next feed/poll. */
const haltz_frame_t *haltz_decoder_frame(const haltz_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_PACKET_H */
