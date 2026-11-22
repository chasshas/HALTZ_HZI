/*
 * HALTZ I/O link messages: structs and pack/unpack functions.
 * Layouts and rules: docs/io-protocol-v1.md section 6. IDs and sizes: msg_ids.def.
 *
 * Same conventions as gpu_msgs.h:
 * pack(msg, buf, cap, out_len)
 *   Checks the values with the same rules as unpack, then writes the payload to buf[0..cap).
 *   *out_len (if not NULL) = payload length. Errors: ARG, BAD_VALUE, NO_SPACE.
 * unpack(msg, buf, len)
 *   Checks the length (BAD_LEN) and the values (BAD_VALUE), then fills *msg.
 *   After an error *msg may be partly written: don't use it.
 *
 * IO_STICK_CAL has no payload and no struct:
 *     haltz_frame_encode(HALTZ_MSG_IO_STICK_CAL, NULL, 0, out, cap, &n);
 */
#ifndef HALTZ_IO_MSGS_H
#define HALTZ_IO_MSGS_H

#include <stddef.h>
#include <stdint.h>

#include "haltz/io.h"
#include "haltz/msgs.h" /* HALTZ_MSG_IO_* */
#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALTZ_IO_NOTES_MAX 126 /* notes per IO_NOTES */

/* ---- I/O board -> S3 --------------------------------------------------------------------- */

typedef struct {
    uint8_t modifiers;                /* HALTZ_IO_MOD_* */
    uint8_t count;                    /* 0..HALTZ_IO_KEYS_MAX */
    uint8_t usages[HALTZ_IO_KEYS_MAX]; /* each 0x04..0xDF, no duplicates */
} haltz_io_keys_t;

typedef struct {
    int16_t x, y;    /* -1000..1000, right = +x, up = +y */
    uint8_t buttons; /* HALTZ_IO_BTN_* */
} haltz_io_stick_t;

typedef struct {
    uint8_t value; /* 0..255 */
} haltz_io_knob_t;

typedef struct {
    uint8_t slot;    /* 0..HALTZ_IO_MAX_DEVICES-1 */
    uint8_t state;   /* HALTZ_IO_DEV_DISCONNECTED / _CONNECTED */
    uint8_t kind;    /* haltz_io_dev_kind_t */
    uint8_t battery; /* 0 unknown, 1 empty .. 255 full */
    uint16_t vendor_id, product_id;
} haltz_io_device_t;

typedef struct {
    uint32_t uptime_ms;   /* going backwards = the board rebooted */
    uint16_t rx_frames;   /* good frames from the S3 (wraps) */
    uint16_t crc_errors;  /* wraps */
    uint16_t cmd_errors;  /* wraps */
    uint16_t rx_overruns; /* wraps; must stay 0 */
    uint8_t flags;        /* HALTZ_IO_STATUS_* (unknown bits are ignored) */
    uint8_t devices;      /* bit n = slot n connected */
    uint8_t tune_free[HALTZ_IO_VOICES];
} haltz_io_status_t;

/* ---- S3 -> I/O board --------------------------------------------------------------------- */

typedef struct {
    uint8_t mode; /* haltz_io_led_mode_t */
    uint8_t r, g, b;
    uint16_t period_ms;
    uint8_t count; /* CODE only (1..15), else 0 */
} haltz_io_led_t;

typedef struct {
    uint8_t voice; /* 0 = A, 1 = B */
    uint16_t hz;   /* 0 = silence, else 20..20000 */
    uint16_t ms;   /* 0 = until stopped */
} haltz_io_tone_t;

typedef struct {
    uint8_t note;  /* 0 rest, 12..127 MIDI */
    uint8_t ticks; /* >= 1 */
} haltz_io_note_t;

typedef struct {
    uint8_t voice;   /* 0 = A, 1 = B */
    uint8_t flags;   /* HALTZ_IO_NOTES_* */
    uint8_t tick_ms; /* >= 1 */
    uint8_t count;   /* 1..HALTZ_IO_NOTES_MAX */
    haltz_io_note_t notes[HALTZ_IO_NOTES_MAX];
} haltz_io_notes_t;

typedef struct {
    uint8_t id;    /* haltz_io_sfx_t */
    uint8_t voice; /* 0 = A, 1 = B */
} haltz_io_sfx_msg_t;

typedef struct {
    uint8_t voices; /* mask: bit 0 A, bit 1 B; at least one bit */
} haltz_io_stop_t;

typedef struct {
    uint8_t voices; /* mask, as for STOP */
    uint8_t level;  /* 0 mute .. 255 full */
} haltz_io_volume_t;

typedef struct {
    uint8_t action; /* haltz_io_bt_action_t */
} haltz_io_bt_t;

/* ---- pack / unpack ----------------------------------------------------------------------- */

#define HALTZ_IO_DECLARE(name, type)                                                           \
    haltz_err_t haltz_io_##name##_pack(const type *msg, uint8_t *buf, size_t cap,              \
                                       size_t *out_len);                                       \
    haltz_err_t haltz_io_##name##_unpack(type *msg, const uint8_t *buf, size_t len);

HALTZ_IO_DECLARE(keys, haltz_io_keys_t)
HALTZ_IO_DECLARE(stick, haltz_io_stick_t)
HALTZ_IO_DECLARE(knob, haltz_io_knob_t)
HALTZ_IO_DECLARE(device, haltz_io_device_t)
HALTZ_IO_DECLARE(status, haltz_io_status_t)
HALTZ_IO_DECLARE(led, haltz_io_led_t)
HALTZ_IO_DECLARE(tone, haltz_io_tone_t)
HALTZ_IO_DECLARE(notes, haltz_io_notes_t)
HALTZ_IO_DECLARE(sfx, haltz_io_sfx_msg_t)
HALTZ_IO_DECLARE(stop, haltz_io_stop_t)
HALTZ_IO_DECLARE(volume, haltz_io_volume_t)
HALTZ_IO_DECLARE(bt, haltz_io_bt_t)

#undef HALTZ_IO_DECLARE

/* ---- any S3 -> I/O command --------------------------------------------------------------- */

/* One decoded command. u is selected by type. IO_STICK_CAL: no u. */
typedef struct {
    uint8_t type; /* HALTZ_MSG_IO_LED .. HALTZ_MSG_IO_BT */
    union {
        haltz_io_led_t led;
        haltz_io_tone_t tone;
        haltz_io_notes_t notes;
        haltz_io_sfx_msg_t sfx;
        haltz_io_stop_t stop;
        haltz_io_volume_t volume;
        haltz_io_bt_t bt;
    } u;
} haltz_io_cmd_t;

/*
 * Unpack any S3 -> I/O command. UNKNOWN_TYPE for other types, including HELLO (handled by
 * the link code) and the I/O -> S3 messages. buf may be NULL when len is 0.
 */
haltz_err_t haltz_io_cmd_unpack(haltz_io_cmd_t *cmd, uint8_t type, const uint8_t *buf,
                                size_t len);

/* The other direction. IO_STICK_CAL packs to 0 bytes (buf may then be NULL). */
haltz_err_t haltz_io_cmd_pack(const haltz_io_cmd_t *cmd, uint8_t *buf, size_t cap,
                              size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_MSGS_H */
