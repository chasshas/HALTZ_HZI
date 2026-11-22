/* HALTZ I/O: status LED patterns. See ledfx.h. */
#include "ledfx.h"

#include <string.h>

static const rgb_t k_black = {0, 0, 0};

void ledfx_init(ledfx_t *f) {
    memset(f, 0, sizeof(*f));
    f->base.mode = HALTZ_IO_LED_SOLID;
}

void ledfx_set(ledfx_t *f, const haltz_io_led_t *m, uint32_t now_ms) {
    if (m->mode == HALTZ_IO_LED_FLASH) {
        f->flash_on = true;
        f->flash = (rgb_t){m->r, m->g, m->b};
        f->flash_until_ms = now_ms + m->period_ms;
        return;
    }
    f->fade_from = f->shown;
    f->base = *m;
    f->base_start_ms = now_ms;
    f->flash_on = false;
}

/* c scaled by level/1024 (level 0..1024). */
static rgb_t scale(rgb_t c, uint32_t level) {
    return (rgb_t){(uint8_t)(c.r * level / 1024), (uint8_t)(c.g * level / 1024),
                   (uint8_t)(c.b * level / 1024)};
}

static uint8_t lerp8(uint8_t a, uint8_t b, uint32_t t, uint32_t span) {
    return (uint8_t)((int32_t)a + ((int32_t)b - (int32_t)a) * (int32_t)t / (int32_t)span);
}

static rgb_t eval_base(const ledfx_t *f, uint32_t now_ms) {
    const haltz_io_led_t *m = &f->base;
    rgb_t c = {m->r, m->g, m->b};
    uint32_t t = now_ms - f->base_start_ms;
    uint32_t p = m->period_ms;
    switch (m->mode) {
    case HALTZ_IO_LED_BLINK:
        return (t % p) < p / 2 ? c : k_black;
    case HALTZ_IO_LED_BREATHE: {
        /* Triangle 0..1024..0 over the period, smoothed (smoothstep), so it eases in and out. */
        uint32_t q = (uint32_t)((uint64_t)(t % p) * 2048 / p); /* 0..2047 */
        uint32_t x = q < 1024 ? q : 2047 - q;                 /* 0..1023 */
        uint32_t level = x * x / 1024 * (3072 - 2 * x) / 1024; /* smoothstep, 0..1023 */
        return scale(c, level);
    }
    case HALTZ_IO_LED_FADE:
        if (p == 0 || t >= p) {
            return c;
        }
        return (rgb_t){lerp8(f->fade_from.r, c.r, t, p), lerp8(f->fade_from.g, c.g, t, p),
                       lerp8(f->fade_from.b, c.b, t, p)};
    case HALTZ_IO_LED_CODE: {
        uint32_t cycle = (uint32_t)(m->count + HALTZ_IO_LED_CODE_PAUSE) * p;
        uint32_t tc = t % cycle;
        return (tc < (uint32_t)m->count * p && (tc % p) < p / 2) ? c : k_black;
    }
    case HALTZ_IO_LED_SOLID:
    default:
        return c;
    }
}

rgb_t ledfx_eval(ledfx_t *f, uint32_t now_ms) {
    rgb_t c;
    if (f->flash_on && (int32_t)(now_ms - f->flash_until_ms) < 0) {
        c = f->flash;
    } else {
        f->flash_on = false;
        c = eval_base(f, now_ms);
    }
    f->shown = c;
    return c;
}
