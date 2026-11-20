#include "haltz/packet.h"

#include <string.h>

#include "haltz/crc8.h"

haltz_err_t haltz_frame_encode(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out,
                               size_t out_cap, size_t *out_len) {
    if (out == NULL || (payload == NULL && len > 0)) {
        return HALTZ_ERR_ARG;
    }
    if (len > HALTZ_MAX_PAYLOAD) {
        return HALTZ_ERR_BAD_LEN;
    }
    if (out_cap < len + HALTZ_FRAME_OVERHEAD) {
        return HALTZ_ERR_NO_SPACE;
    }

    uint8_t crc = HALTZ_CRC8_INIT;
    size_t n = 0;
    out[n++] = HALTZ_SYNC;
    out[n++] = type;
    crc = haltz_crc8_update(crc, type);
    out[n++] = (uint8_t)len;
    crc = haltz_crc8_update(crc, (uint8_t)len);
    for (size_t i = 0; i < len; i++) {
        out[n++] = payload[i];
        crc = haltz_crc8_update(crc, payload[i]);
    }
    out[n++] = crc;

    if (out_len != NULL) {
        *out_len = n;
    }
    return HALTZ_OK;
}

/* ---- decoder ---------------------------------------------------------------------------- */

enum {
    ST_HUNT = 0, /* waiting for SYNC */
    ST_TYPE,
    ST_LEN,
    ST_PAYLOAD,
    ST_CRC,
};

void haltz_decoder_init(haltz_decoder_t *dec) {
    memset(dec, 0, sizeof(*dec));
    dec->state = ST_HUNT;
}

const haltz_frame_t *haltz_decoder_frame(const haltz_decoder_t *dec) {
    return &dec->frame;
}

static void start_hunt(haltz_decoder_t *dec) {
    dec->state = ST_HUNT;
    dec->raw_len = 0;
    dec->crc = HALTZ_CRC8_INIT;
}

/*
 * The candidate in raw[] failed its CRC. Everything after its SYNC byte might hold the start
 * of a real frame, so put raw[k..] (k = next SYNC after index 0) in front of the backlog.
 */
static void resync_after_bad_frame(haltz_decoder_t *dec) {
    uint16_t k = 1;
    while (k < dec->raw_len && dec->raw[k] != HALTZ_SYNC) {
        k++;
    }
    uint16_t replay = (uint16_t)(dec->raw_len - k);
    dec->bytes_skipped += k;

    if (replay > 0) {
        /* Never more than HALTZ_MAX_FRAME bytes are pending in total (see packet.h); the
         * check only guards against misuse such as a corrupted decoder struct. */
        if ((size_t)replay + dec->backlog_len > sizeof(dec->backlog)) {
            dec->bytes_skipped += dec->backlog_len;
            dec->backlog_len = 0;
        }
        memmove(&dec->backlog[replay], &dec->backlog[dec->backlog_head], dec->backlog_len);
        memcpy(&dec->backlog[0], &dec->raw[k], replay);
        dec->backlog_head = 0;
        dec->backlog_len = (uint16_t)(dec->backlog_len + replay);
    }
    start_hunt(dec);
}

/* Run the state machine on one byte. */
static haltz_dec_result_t step(haltz_decoder_t *dec, uint8_t byte) {
    if (dec->state == ST_HUNT) {
        if (byte != HALTZ_SYNC) {
            dec->bytes_skipped++;
            return HALTZ_DEC_NEED_MORE;
        }
        dec->raw[0] = byte;
        dec->raw_len = 1;
        dec->crc = HALTZ_CRC8_INIT;
        dec->state = ST_TYPE;
        return HALTZ_DEC_NEED_MORE;
    }

    dec->raw[dec->raw_len++] = byte;

    switch (dec->state) {
    case ST_TYPE:
        dec->crc = haltz_crc8_update(dec->crc, byte);
        dec->state = ST_LEN;
        break;
    case ST_LEN:
        dec->crc = haltz_crc8_update(dec->crc, byte);
        dec->state = (byte == 0) ? ST_CRC : ST_PAYLOAD;
        break;
    case ST_PAYLOAD:
        dec->crc = haltz_crc8_update(dec->crc, byte);
        if (dec->raw_len == 3u + dec->raw[2]) {
            dec->state = ST_CRC;
        }
        break;
    case ST_CRC:
        if (byte == dec->crc) {
            dec->frame.type = dec->raw[1];
            dec->frame.len = dec->raw[2];
            memcpy(dec->frame.payload, &dec->raw[3], dec->raw[2]);
            dec->frames_ok++;
            start_hunt(dec);
            return HALTZ_DEC_FRAME;
        }
        dec->crc_errors++;
        resync_after_bad_frame(dec);
        return HALTZ_DEC_CRC_ERROR;
    default:
        start_hunt(dec);
        break;
    }
    return HALTZ_DEC_NEED_MORE;
}

haltz_dec_result_t haltz_decoder_poll(haltz_decoder_t *dec) {
    while (dec->backlog_len > 0) {
        uint8_t b = dec->backlog[dec->backlog_head++];
        dec->backlog_len--;
        if (dec->backlog_len == 0) {
            dec->backlog_head = 0;
        }
        haltz_dec_result_t r = step(dec, b);
        if (r != HALTZ_DEC_NEED_MORE) {
            return r;
        }
    }
    return HALTZ_DEC_NEED_MORE;
}

haltz_dec_result_t haltz_decoder_flush(haltz_decoder_t *dec) {
    if (dec->state != ST_HUNT && dec->raw_len > 0) {
        dec->flushes++;
        resync_after_bad_frame(dec);
    }
    return haltz_decoder_poll(dec);
}

haltz_dec_result_t haltz_decoder_feed(haltz_decoder_t *dec, uint8_t byte) {
    if (dec->backlog_len == 0) {
        return step(dec, byte); /* fast path: nothing held back */
    }
    /* Held-back bytes come first; queue the new byte behind them. */
    if (dec->backlog_head + dec->backlog_len >= sizeof(dec->backlog)) {
        memmove(&dec->backlog[0], &dec->backlog[dec->backlog_head], dec->backlog_len);
        dec->backlog_head = 0;
    }
    if (dec->backlog_len < sizeof(dec->backlog)) {
        dec->backlog[dec->backlog_head + dec->backlog_len] = byte;
        dec->backlog_len++;
    } else {
        dec->bytes_skipped++; /* cannot happen with correct use, see packet.h */
    }
    return haltz_decoder_poll(dec);
}
