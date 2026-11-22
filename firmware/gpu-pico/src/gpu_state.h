/*
 * GPU picture state (docs/gpu-protocol-v1.md section 3) and the commands that change it.
 *
 * Portable C, no Pico SDK calls: the S3 fallback renderer and the PC tests use it too.
 * One owner at a time: commands change the state on core 0 only while core 1 is not
 * rendering it (see main.c).
 *
 * Dirty tracking: every change marks the 8 x 8 screen cells it touches. At PRESENT the cells
 * are turned into rectangles (gpu_state_take_dirty) and only those are drawn.
 */
#ifndef HALTZ_GPU_STATE_H
#define HALTZ_GPU_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "haltz/gpu.h"
#include "haltz/gpu_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_CELL      8 /* dirty-tracking cell = text cell */
#define GPU_CELLS_X   (HALTZ_GPU_SCREEN_W / GPU_CELL) /* 40 */
#define GPU_CELLS_Y   (HALTZ_GPU_SCREEN_H / GPU_CELL) /* 30 */
#define GPU_MAX_RECTS 48 /* more dirty rectangles than this: redraw the whole screen */

typedef struct {
    int16_t x, y, w, h; /* pixels */
} gpu_rect_t;

typedef struct {
    int16_t x, y;
    uint8_t pattern;
    uint8_t attr; /* HALTZ_GPU_SPR_* */
} gpu_sprite_t;

typedef struct {
    uint16_t palette[HALTZ_GPU_PALETTE_SIZE];
    uint8_t font[HALTZ_GPU_FONT_GLYPHS][HALTZ_GPU_GLYPH_BYTES];
    uint8_t layers; /* HALTZ_GPU_LAYER_* */

    uint8_t text_ch[HALTZ_GPU_TEXT_ROWS][HALTZ_GPU_TEXT_COLS];
    uint8_t text_fg[HALTZ_GPU_TEXT_ROWS][HALTZ_GPU_TEXT_COLS];
    uint8_t text_bg[HALTZ_GPU_TEXT_ROWS][HALTZ_GPU_TEXT_COLS];
    uint8_t cursor_col, cursor_row, cursor_style;

    uint8_t patterns[HALTZ_GPU_PATTERNS][HALTZ_GPU_PATTERN_BYTES];
    uint16_t pattern_rows[HALTZ_GPU_PATTERNS]; /* derived: bit r = row r has a visible pixel */
    uint16_t map[HALTZ_GPU_MAP_H][HALTZ_GPU_MAP_W];
    uint16_t scroll_x, scroll_y; /* 0..511 */

    gpu_sprite_t sprites[HALTZ_GPU_SPRITES];

    uint8_t bitmap[HALTZ_GPU_SCREEN_H][HALTZ_GPU_SCREEN_W];

    uint64_t dirty[GPU_CELLS_Y]; /* bit c of dirty[r]: cell (c, r) changed since last take */
} gpu_state_t;

/* GPU_INIT: everything back to the defaults, whole screen dirty. */
void gpu_state_init(gpu_state_t *s);

/*
 * Apply one decoded command. Returns false for commands that are not picture changes
 * (GPU_PRESENT, GPU_STATUS_REQ, GPU_BACKLIGHT): the caller handles those.
 * The command must already be valid (haltz_gpu_cmd_unpack checked it).
 */
bool gpu_state_apply(gpu_state_t *s, const haltz_gpu_cmd_t *cmd);

/* Mark a pixel rectangle dirty (clipped to the screen; w or h <= 0 does nothing). */
void gpu_state_mark(gpu_state_t *s, int x, int y, int w, int h);
void gpu_state_mark_all(gpu_state_t *s);
bool gpu_state_is_dirty(const gpu_state_t *s);

/*
 * Turn the dirty cells into at most max rectangles (pixels) and clear them. Returns the
 * number written. If the cells need more than max rectangles, or most of the screen is
 * dirty, the result is one full-screen rectangle.
 */
int gpu_state_take_dirty(gpu_state_t *s, gpu_rect_t *rects, int max);

/* Pixel rectangle of the text cursor cell. False if the cursor is off or off screen. */
bool gpu_state_cursor_rect(const gpu_state_t *s, gpu_rect_t *r);
/* True if the cursor is on and blinks (so it needs redrawing on its own). */
bool gpu_state_cursor_blinks(const gpu_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_GPU_STATE_H */
