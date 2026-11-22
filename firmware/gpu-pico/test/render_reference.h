/* Reference renderer for the PC tests (see render_reference.c). */
#ifndef HALTZ_GPU_RENDER_REFERENCE_H
#define HALTZ_GPU_RENDER_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu_render.h"
#include "gpu_state.h"

void ref_render_rect(const gpu_state_t *s, int x, int y, int w, int h, bool blink_on,
                     uint16_t *out);

#endif
