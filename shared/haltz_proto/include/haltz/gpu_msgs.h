/*
 * HALTZ GPU link messages: structs and pack/unpack functions.
 * Layouts and rules: docs/gpu-protocol-v1.md section 4. IDs and sizes: msg_ids.def.
 *
 * pack(msg, buf, cap, out_len)
 *   Checks the values with the same rules as unpack, then writes the payload to buf[0..cap).
 *   *out_len (if not NULL) = payload length. Errors: ARG, BAD_VALUE, NO_SPACE.
 * unpack(msg, buf, len)
 *   Checks the length (BAD_LEN) and the values (BAD_VALUE), then fills *msg.
 *   After an error *msg may be partly written: don't use it.
 *
 * A receiver answers BAD_LEN / BAD_VALUE with a NAK and ignores the command. Drawing outside
 * the screen is not an error (it is clipped), so most coordinates are not checked here.
 * Variable-length data is copied into the struct, so no pointer into buf is kept.
 *
 * GPU_INIT and GPU_STATUS_REQ have no payload and no struct:
 *     haltz_frame_encode(HALTZ_MSG_GPU_INIT, NULL, 0, out, cap, &n);
 */
#ifndef HALTZ_GPU_MSGS_H
#define HALTZ_GPU_MSGS_H

#include <stddef.h>
#include <stdint.h>

#include "haltz/gpu.h"
#include "haltz/msgs.h" /* HALTZ_MSG_GPU_* */
#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Most elements one message can carry (payload is at most 255 bytes). */
#define HALTZ_GPU_PALETTE_MAX    127 /* colours per GPU_PALETTE */
#define HALTZ_GPU_TEXT_PUT_MAX   251 /* characters per GPU_TEXT_PUT */
#define HALTZ_GPU_FONT_LOAD_MAX  31  /* glyphs per GPU_FONT_LOAD */
#define HALTZ_GPU_TILE_PUT_MAX   126 /* map entries per GPU_TILE_PUT */
#define HALTZ_GPU_BMP_PIXELS_MAX 251 /* pixels per GPU_BMP_PIXELS */

/* ---- control ----------------------------------------------------------------------------- */

typedef struct {
    uint16_t id; /* chosen by the S3, reported back in GPU_STATUS.present_id */
} haltz_gpu_present_t;

typedef struct {
    uint8_t level; /* 0 off .. 255 full */
} haltz_gpu_backlight_t;

typedef struct {
    uint8_t mask; /* HALTZ_GPU_LAYER_* bits, others must be 0 */
} haltz_gpu_layers_t;

typedef struct {
    uint8_t first;                        /* first palette index */
    uint8_t count;                        /* 1..127, first + count <= 256 */
    uint16_t colors[HALTZ_GPU_PALETTE_MAX]; /* RGB565 */
} haltz_gpu_palette_t;

/* ---- text layer -------------------------------------------------------------------------- */

typedef struct {
    uint8_t col; /* < 40 */
    uint8_t row; /* < 30 */
    uint8_t fg;
    uint8_t bg;
    uint8_t count; /* 1..251 */
    uint8_t chars[HALTZ_GPU_TEXT_PUT_MAX];
} haltz_gpu_text_put_t;

typedef struct {
    uint8_t col, row, w, h; /* clipped to the screen */
    uint8_t ch, fg, bg;
} haltz_gpu_text_fill_t;

typedef struct {
    uint8_t top, height; /* clipped to the screen */
    int8_t rows;         /* > 0 moves up, < 0 moves down */
    uint8_t ch, fg, bg;  /* fills the freed rows */
} haltz_gpu_text_scroll_t;

typedef struct {
    uint8_t col, row; /* outside the screen = hidden */
    uint8_t style;    /* HALTZ_GPU_CURSOR_OFF / _UNDERLINE / _BLOCK, optionally | _BLINK */
} haltz_gpu_text_cursor_t;

typedef struct {
    uint8_t first; /* first glyph */
    uint8_t count; /* 1..31, first + count <= 256 */
    uint8_t glyphs[HALTZ_GPU_FONT_LOAD_MAX][HALTZ_GPU_GLYPH_BYTES];
} haltz_gpu_font_load_t;

/* ---- patterns, tile layer, sprites ------------------------------------------------------- */

typedef struct {
    uint8_t index;
    uint8_t data[HALTZ_GPU_PATTERN_BYTES]; /* 16 rows x 8 bytes, high nibble = left pixel */
} haltz_gpu_pattern_load_t;

typedef struct {
    uint8_t x, y;  /* < 32 */
    uint8_t count; /* 1..126 */
    uint16_t entries[HALTZ_GPU_TILE_PUT_MAX]; /* HALTZ_GPU_TILE(); reserved bits must be 0 */
} haltz_gpu_tile_put_t;

typedef struct {
    uint8_t x, y, w, h; /* clipped to the map */
    uint16_t entry;     /* reserved bits must be 0 */
} haltz_gpu_tile_fill_t;

typedef struct {
    uint16_t sx, sy; /* map pixel shown at screen (0, 0), taken mod 512 */
} haltz_gpu_tile_scroll_t;

typedef struct {
    uint8_t id;     /* < 64 */
    int16_t x, y;   /* top-left on screen, may be off screen */
    uint8_t pattern;
    uint8_t attr;   /* HALTZ_GPU_SPR_* */
} haltz_gpu_sprite_set_t;

/* ---- bitmap layer ------------------------------------------------------------------------ */

typedef struct {
    uint16_t x, y;
    uint8_t count; /* 1..251 */
    uint8_t pixels[HALTZ_GPU_BMP_PIXELS_MAX];
} haltz_gpu_bmp_pixels_t;

typedef struct {
    uint16_t x, y, w, h;
    uint8_t color;
} haltz_gpu_bmp_fill_t;

typedef struct {
    int16_t x0, y0, x1, y1; /* both ends included */
    uint8_t color;
} haltz_gpu_bmp_line_t;

typedef struct {
    uint16_t sx, sy, w, h; /* source rectangle */
    int16_t dx, dy;        /* destination top-left */
} haltz_gpu_bmp_copy_t;

/* ---- GPU -> S3 --------------------------------------------------------------------------- */

typedef struct {
    uint16_t present_id;  /* last PRESENT that is on the panel */
    uint16_t frames;      /* frames shown since boot (wraps) */
    uint16_t render_us;   /* time of the last frame, saturates at 65535 */
    uint8_t rx_free_kb;   /* free receive ring space */
    uint8_t flags;        /* HALTZ_GPU_STATUS_* (unknown bits are ignored) */
    uint16_t crc_errors;  /* wraps */
    uint16_t cmd_errors;  /* wraps */
    uint16_t rx_overruns; /* wraps; must stay 0 */
    uint32_t uptime_ms;   /* going backwards = the GPU rebooted */
} haltz_gpu_status_t;

/* ---- pack / unpack ----------------------------------------------------------------------- */

#define HALTZ_GPU_DECLARE(name)                                                                \
    haltz_err_t haltz_gpu_##name##_pack(const haltz_gpu_##name##_t *msg, uint8_t *buf,         \
                                        size_t cap, size_t *out_len);                          \
    haltz_err_t haltz_gpu_##name##_unpack(haltz_gpu_##name##_t *msg, const uint8_t *buf,       \
                                          size_t len);

HALTZ_GPU_DECLARE(present)
HALTZ_GPU_DECLARE(backlight)
HALTZ_GPU_DECLARE(layers)
HALTZ_GPU_DECLARE(palette)
HALTZ_GPU_DECLARE(text_put)
HALTZ_GPU_DECLARE(text_fill)
HALTZ_GPU_DECLARE(text_scroll)
HALTZ_GPU_DECLARE(text_cursor)
HALTZ_GPU_DECLARE(font_load)
HALTZ_GPU_DECLARE(pattern_load)
HALTZ_GPU_DECLARE(tile_put)
HALTZ_GPU_DECLARE(tile_fill)
HALTZ_GPU_DECLARE(tile_scroll)
HALTZ_GPU_DECLARE(sprite_set)
HALTZ_GPU_DECLARE(bmp_pixels)
HALTZ_GPU_DECLARE(bmp_fill)
HALTZ_GPU_DECLARE(bmp_line)
HALTZ_GPU_DECLARE(bmp_copy)
HALTZ_GPU_DECLARE(status)

#undef HALTZ_GPU_DECLARE

/* ---- any S3 -> GPU command --------------------------------------------------------------- */

/* One decoded drawing/control command. u is selected by type. GPU_INIT, GPU_STATUS_REQ: no u. */
typedef struct {
    uint8_t type; /* HALTZ_MSG_GPU_* */
    union {
        haltz_gpu_present_t present;
        haltz_gpu_backlight_t backlight;
        haltz_gpu_layers_t layers;
        haltz_gpu_palette_t palette;
        haltz_gpu_text_put_t text_put;
        haltz_gpu_text_fill_t text_fill;
        haltz_gpu_text_scroll_t text_scroll;
        haltz_gpu_text_cursor_t text_cursor;
        haltz_gpu_font_load_t font_load;
        haltz_gpu_pattern_load_t pattern_load;
        haltz_gpu_tile_put_t tile_put;
        haltz_gpu_tile_fill_t tile_fill;
        haltz_gpu_tile_scroll_t tile_scroll;
        haltz_gpu_sprite_set_t sprite_set;
        haltz_gpu_bmp_pixels_t bmp_pixels;
        haltz_gpu_bmp_fill_t bmp_fill;
        haltz_gpu_bmp_line_t bmp_line;
        haltz_gpu_bmp_copy_t bmp_copy;
    } u;
} haltz_gpu_cmd_t;

/*
 * Unpack any S3 -> GPU command (everything in the GPU range except GPU_STATUS).
 * Returns UNKNOWN_TYPE for other types, including HELLO (handled by the link code) and
 * GPU_STATUS (it goes the other way). buf may be NULL when len is 0.
 */
haltz_err_t haltz_gpu_cmd_unpack(haltz_gpu_cmd_t *cmd, uint8_t type, const uint8_t *buf,
                                 size_t len);

/*
 * The other direction: pack cmd->u (selected by cmd->type) into buf, like the per-message
 * pack functions. GPU_INIT and GPU_STATUS_REQ pack to 0 bytes (buf may then be NULL).
 * UNKNOWN_TYPE for types haltz_gpu_cmd_unpack does not accept.
 */
haltz_err_t haltz_gpu_cmd_pack(const haltz_gpu_cmd_t *cmd, uint8_t *buf, size_t cap,
                               size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_GPU_MSGS_H */
