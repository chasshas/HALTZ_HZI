/*
 * GPU compositor: turns the picture state into RGB565 pixels for any rectangle.
 * Portable C, no Pico SDK calls (used by the PC tests, and later by the S3 fallback).
 */
#ifndef HALTZ_GPU_RENDER_H
#define HALTZ_GPU_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compose the rectangle (x, y, w, h), which must lie on screen, into out[w * h] RGB565
 * values, row by row. Layers back to front: backdrop, bitmap, tiles, sprites, text (with
 * cursor), sprites marked ABOVE_TEXT.
 * blink_on: blink phase of the text cursor. A blinking cursor shows only when true; a
 * steady cursor always shows.
 */
void gpu_render_rect(const gpu_state_t *s, int x, int y, int w, int h, bool blink_on,
                     uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_GPU_RENDER_H */
