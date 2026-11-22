/*
 * Built-in pictures, drawn with the same commands the S3 sends (portable C).
 *   Boot screen: shown until the CPU takes over (docs/gpu-protocol-v1.md section 2).
 *   Demo: an animated scene using every layer, started from the console ('d'), for
 *   checking the renderer on the real panel before the S3 driver exists.
 */
#ifndef HALTZ_GPU_SCENES_H
#define HALTZ_GPU_SCENES_H

#include <stdint.h>

#include "gpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* git_hash: 8 hex characters. */
void gpu_scene_boot(gpu_state_t *s, unsigned proto_version, const char *git_hash);

void gpu_scene_demo_init(gpu_state_t *s);
void gpu_scene_demo_step(gpu_state_t *s, uint32_t frame); /* then render the dirty areas */

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_GPU_SCENES_H */
