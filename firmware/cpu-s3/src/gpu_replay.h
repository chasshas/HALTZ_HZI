/*
 * HALTZ CPU: turn a GPU picture state (the S3's shadow) back into commands.
 *
 * After a GPU reset the driver sends GPU_INIT and then these commands; applied to a fresh
 * GPU_INIT state they give the same picture again (palette, font, layers, text, cursor,
 * patterns, tile map, scroll, sprites, bitmap). Only what differs from the GPU_INIT defaults
 * is sent, so a text-only screen costs a few hundred bytes.
 *
 * Portable C (no ESP-IDF): the PC test in firmware/cpu-s3/test checks it on random pictures.
 */
#ifndef HALTZ_CPU_GPU_REPLAY_H
#define HALTZ_CPU_GPU_REPLAY_H

#include "haltz/gpu_msgs.h"

#include "../../gpu-pico/src/gpu_state.h" /* the GPU firmware's picture state */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*gpu_replay_emit_fn)(const haltz_gpu_cmd_t *cmd, void *ctx);

/* Calls emit once per command, in order. GPU_INIT itself is not included. scratch is
 * caller-provided (the command union is ~260 bytes; keep it off small stacks). */
void gpu_replay(const gpu_state_t *s, haltz_gpu_cmd_t *scratch, gpu_replay_emit_fn emit,
                void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_GPU_REPLAY_H */
