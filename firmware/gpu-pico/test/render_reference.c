/*
 * Reference renderer for the PC tests: the first, simple per-pixel version of gpu_render.c.
 * The real renderer is optimised; test_render.c checks that both give the same pixels.
 * Keep this one simple and obviously right; don't optimise it.
 */
#include "render_reference.h"

#include <string.h>

#define W        HALTZ_GPU_SCREEN_W
#define TILE     HALTZ_GPU_PATTERN_SIZE
#define MAP_MASK (HALTZ_GPU_MAP_PX - 1)

/* Pixel p (0..15) of a 4-bit pattern at (px, py), 0..15 each. */
static inline uint8_t ref_pattern_pixel(const uint8_t *pat, int px, int py) {
    uint8_t b = pat[py * (TILE / 2) + (px >> 1)];
    return (px & 1) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
}

/* Tiles for one row segment: screen columns x0 .. x0+n-1 on screen row y. */
static void draw_tiles(const gpu_state_t *s, int x0, int n, int y, uint8_t *idx) {
    int my = (y + s->scroll_y) & MAP_MASK;
    const uint16_t *map_row = s->map[my / TILE];
    int py = my % TILE;
    for (int i = 0; i < n; i++) {
        int mx = (x0 + i + s->scroll_x) & MAP_MASK;
        uint16_t e = map_row[mx / TILE];
        int px = mx % TILE;
        if (e & HALTZ_GPU_TILE_HFLIP) {
            px = TILE - 1 - px;
        }
        int ty = (e & HALTZ_GPU_TILE_VFLIP) ? TILE - 1 - py : py;
        uint8_t p = ref_pattern_pixel(s->patterns[e & HALTZ_GPU_TILE_PATTERN_MASK], px, ty);
        if (p != 0) {
            idx[i] = (uint8_t)(((e & HALTZ_GPU_TILE_BANK_MASK) >> HALTZ_GPU_TILE_BANK_SHIFT) << 4 |
                               p);
        }
    }
}

/* Sprites for one row segment; above = which group (ABOVE_TEXT set or not). */
static void draw_sprites(const gpu_state_t *s, int x0, int n, int y, bool above, uint8_t *idx) {
    for (int id = 0; id < HALTZ_GPU_SPRITES; id++) { /* higher id drawn later = on top */
        const gpu_sprite_t *sp = &s->sprites[id];
        if (!(sp->attr & HALTZ_GPU_SPR_VISIBLE) ||
            !!(sp->attr & HALTZ_GPU_SPR_ABOVE_TEXT) != above) {
            continue;
        }
        int py = y - sp->y;
        if (py < 0 || py >= TILE) {
            continue;
        }
        if (sp->attr & HALTZ_GPU_SPR_VFLIP) {
            py = TILE - 1 - py;
        }
        int a = sp->x > x0 ? sp->x : x0; /* overlap of sprite and segment, screen x */
        int b = (sp->x + TILE < x0 + n) ? sp->x + TILE : x0 + n;
        const uint8_t *pat = s->patterns[sp->pattern];
        uint8_t bank = (uint8_t)((sp->attr & HALTZ_GPU_SPR_BANK_MASK) << 4);
        for (int sx = a; sx < b; sx++) {
            int px = sx - sp->x;
            if (sp->attr & HALTZ_GPU_SPR_HFLIP) {
                px = TILE - 1 - px;
            }
            uint8_t p = ref_pattern_pixel(pat, px, py);
            if (p != 0) {
                idx[sx - x0] = bank | p;
            }
        }
    }
}

/* Text cells (and the cursor) for one row segment. */
static void draw_text(const gpu_state_t *s, int x0, int n, int y, bool blink_on, uint8_t *idx) {
    int row = y / HALTZ_GPU_GLYPH_SIZE;
    int gy = y % HALTZ_GPU_GLYPH_SIZE;
    uint8_t shape = (uint8_t)(s->cursor_style & ~HALTZ_GPU_CURSOR_BLINK);
    bool cursor_shown =
        shape != HALTZ_GPU_CURSOR_OFF && s->cursor_row == row &&
        (!(s->cursor_style & HALTZ_GPU_CURSOR_BLINK) || blink_on);
    for (int i = 0; i < n; i++) {
        int x = x0 + i;
        int col = x / HALTZ_GPU_GLYPH_SIZE;
        uint8_t fg = s->text_fg[row][col];
        uint8_t bg = s->text_bg[row][col];
        bool on = (s->font[s->text_ch[row][col]][gy] >> (7 - (x % HALTZ_GPU_GLYPH_SIZE))) & 1u;
        uint8_t c = on ? fg : bg;
        if (cursor_shown && col == s->cursor_col) {
            if (shape == HALTZ_GPU_CURSOR_BLOCK) {
                c = on ? bg : fg; /* inverse cell */
            } else if (gy == HALTZ_GPU_GLYPH_SIZE - 1) {
                c = fg; /* underline on the bottom row */
            }
        }
        if (c != HALTZ_GPU_TRANSPARENT) {
            idx[i] = c;
        }
    }
}

void ref_render_rect(const gpu_state_t *s, int x, int y, int w, int h, bool blink_on,
                     uint16_t *out) {
    uint8_t idx[W];
    for (int r = 0; r < h; r++) {
        int sy = y + r;
        if (s->layers & HALTZ_GPU_LAYER_BITMAP) {
            memcpy(idx, &s->bitmap[sy][x], (size_t)w);
        } else {
            memset(idx, HALTZ_GPU_TRANSPARENT, (size_t)w);
        }
        if (s->layers & HALTZ_GPU_LAYER_TILES) {
            draw_tiles(s, x, w, sy, idx);
        }
        if (s->layers & HALTZ_GPU_LAYER_SPRITES) {
            draw_sprites(s, x, w, sy, false, idx);
        }
        if (s->layers & HALTZ_GPU_LAYER_TEXT) {
            draw_text(s, x, w, sy, blink_on, idx);
        }
        if (s->layers & HALTZ_GPU_LAYER_SPRITES) {
            draw_sprites(s, x, w, sy, true, idx);
        }
        uint16_t *o = &out[r * w];
        for (int i = 0; i < w; i++) {
            o[i] = s->palette[idx[i]]; /* index 0 = palette[0] = backdrop */
        }
    }
}
