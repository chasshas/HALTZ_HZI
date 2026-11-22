/*
 * HALTZ CPU: GPU demo scene, driven from the S3 (docs/gpu-protocol-v1.md section 9, step 5).
 *
 * A night scene that uses every layer and every drawing message over the link:
 *   bitmap   sky bands (BMP_FILL), aurora (BMP_PIXELS rows), mountains (BMP_LINE), moon,
 *            stars that twinkle by palette animation (GPU_PALETTE), and a frame-rate graph
 *            that scrolls with BMP_COPY
 *   tiles    grass, dirt and bushes, scrolling sideways (TILE_FILL, TILE_PUT, TILE_SCROLL)
 *   sprites  bouncing balls in several palette banks, a running figure (pattern swap), a UFO,
 *            and the pointer above the text (all from PATTERN_LOAD)
 *   text     title and status bars with custom glyphs (FONT_LOAD)
 *
 * Portable C (no ESP-IDF): commands go out through a callback, as in gpu_replay.c. The PC test
 * firmware/cpu-s3/test/test_demo.c checks every command and can write PNG frames.
 */
#ifndef HALTZ_CPU_DEMO_SCENE_H
#define HALTZ_CPU_DEMO_SCENE_H

#include <stdint.h>

#include "haltz/gpu_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*demo_emit_fn)(const haltz_gpu_cmd_t *cmd, void *ctx);

/* GPU_INIT, then everything that doesn't move. About 40 KB on the wire. */
void demo_scene_init(demo_emit_fn emit, void *ctx);

/* One frame's changes (then the caller sends GPU_PRESENT). fps >= 0: a new frame-rate sample
 * (once a second): shown as text and as a new bar in the graph. */
void demo_scene_step(uint32_t frame, int fps, demo_emit_fn emit, void *ctx);

/* Sprites the scene shows. */
#define DEMO_SCENE_SPRITES 43

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_DEMO_SCENE_H */
