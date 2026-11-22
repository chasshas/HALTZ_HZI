/*
 * HALTZ I/O: status LED patterns (docs/io-protocol-v1.md section 5).
 * Portable C (no ESP-IDF calls), tested on the PC. io_fx.c evaluates the pattern every 10 ms
 * and applies the gamma curve and the PWM. Not thread safe: the caller locks.
 */
#ifndef HALTZ_IO_LEDFX_H
#define HALTZ_IO_LEDFX_H

#include <stdbool.h>
#include <stdint.h>

#include "haltz/io.h"
#include "haltz/io_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef struct {
    haltz_io_led_t base; /* the running pattern (never FLASH) */
    uint32_t base_start_ms;
    rgb_t fade_from;
    bool flash_on;
    rgb_t flash;
    uint32_t flash_until_ms;
    rgb_t shown; /* last result of ledfx_eval() */
} ledfx_t;

/* Starts dark (SOLID black). */
void ledfx_init(ledfx_t *f);
/* IO_LED (already value-checked). FLASH shows over the running pattern; any other mode
 * replaces the running pattern and ends a FLASH that is still showing. */
void ledfx_set(ledfx_t *f, const haltz_io_led_t *m, uint32_t now_ms);
/* The colour to show now (before gamma). Remembers it as "what is showing" for FADE. */
rgb_t ledfx_eval(ledfx_t *f, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_LEDFX_H */
