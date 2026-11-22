#include "gpu_render.h"

#include <string.h>

/*
 * Speed notes (the Pico draws every changed pixel of every frame):
 *   - On the Pico this code runs from RAM (CMake defines GPU_RENDER_IN_RAM=1): the 16 KB
 *     flash cache is shared with core 0, whose link and USB code would push it out.
 *   - Work is done per run, not per pixel: one tile map entry per run of up to 16 pixels,
 *     one glyph row per 8-pixel text cell, sprites filtered once per strip.
 *   - Rows of a pattern that hold no visible pixel are skipped (s->pattern_rows).
 * test/test_render.c compares every result with the simple reference renderer in
 * test/render_reference.c, so keep both giving the same pixels.
 */
#if defined(GPU_RENDER_IN_RAM) && GPU_RENDER_IN_RAM
#define HOT __attribute__((section(".time_critical.gpu_render")))
#else
#define HOT
#endif

#define W        HALTZ_GPU_SCREEN_W
#define TILE     HALTZ_GPU_PATTERN_SIZE
#define MAP_MASK (HALTZ_GPU_MAP_PX - 1)
#define GLYPH    HALTZ_GPU_GLYPH_SIZE

/* The 16 pixel values (0..15) of one pattern row. */
static inline void HOT unpack_row(const uint8_t *row, uint8_t pix[TILE]) {
    for (int b = 0; b < TILE / 2; b++) {
        pix[2 * b] = (uint8_t)(row[b] >> 4);
        pix[2 * b + 1] = (uint8_t)(row[b] & 0x0Fu);
    }
}

/* ---- tiles ------------------------------------------------------------------------------- */

static void HOT draw_tiles(const gpu_state_t *s, int x0, int n, int y, uint8_t *idx) {
    int my = (y + s->scroll_y) & MAP_MASK;
    const uint16_t *map_row = s->map[my / TILE];
    int py = my % TILE;
    int i = 0;
    while (i < n) {
        int mx = (x0 + i + s->scroll_x) & MAP_MASK;
        int px = mx % TILE;
        int run = TILE - px; /* pixels left in this tile */
        if (run > n - i) {
            run = n - i;
        }
        uint16_t e = map_row[mx / TILE];
        uint8_t pat = (uint8_t)(e & HALTZ_GPU_TILE_PATTERN_MASK);
        int ty = (e & HALTZ_GPU_TILE_VFLIP) ? TILE - 1 - py : py;
        if ((s->pattern_rows[pat] >> ty) & 1u) {
            uint8_t pix[TILE];
            unpack_row(&s->patterns[pat][ty * (TILE / 2)], pix);
            uint8_t bank = (uint8_t)(((e & HALTZ_GPU_TILE_BANK_MASK) >> HALTZ_GPU_TILE_BANK_SHIFT)
                                     << 4);
            uint8_t *out = &idx[i];
            if (e & HALTZ_GPU_TILE_HFLIP) {
                for (int k = 0; k < run; k++) {
                    uint8_t p = pix[TILE - 1 - (px + k)];
                    if (p != 0) {
                        out[k] = bank | p;
                    }
                }
            } else {
                for (int k = 0; k < run; k++) {
                    uint8_t p = pix[px + k];
                    if (p != 0) {
                        out[k] = bank | p;
                    }
                }
            }
        }
        i += run;
    }
}

/* ---- sprites ----------------------------------------------------------------------------- */

typedef struct {
    int n;
    uint8_t id[HALTZ_GPU_SPRITES]; /* ascending: later ones are drawn on top */
} sprite_list_t;

/* Visible sprites of one group that touch the rectangle (x, y, w, h). */
static void HOT find_sprites(const gpu_state_t *s, int x, int y, int w, int h, bool above,
                             sprite_list_t *list) {
    list->n = 0;
    for (int id = 0; id < HALTZ_GPU_SPRITES; id++) {
        const gpu_sprite_t *sp = &s->sprites[id];
        if ((sp->attr & HALTZ_GPU_SPR_VISIBLE) &&
            !!(sp->attr & HALTZ_GPU_SPR_ABOVE_TEXT) == above && sp->x < x + w &&
            sp->x + TILE > x && sp->y < y + h && sp->y + TILE > y &&
            s->pattern_rows[sp->pattern] != 0) {
            list->id[list->n++] = (uint8_t)id;
        }
    }
}

static void HOT draw_sprites(const gpu_state_t *s, const sprite_list_t *list, int x0, int n,
                             int y, uint8_t *idx) {
    for (int k = 0; k < list->n; k++) {
        const gpu_sprite_t *sp = &s->sprites[list->id[k]];
        int py = y - sp->y;
        if (py < 0 || py >= TILE) {
            continue;
        }
        if (sp->attr & HALTZ_GPU_SPR_VFLIP) {
            py = TILE - 1 - py;
        }
        if (!((s->pattern_rows[sp->pattern] >> py) & 1u)) {
            continue;
        }
        uint8_t pix[TILE];
        unpack_row(&s->patterns[sp->pattern][py * (TILE / 2)], pix);
        int a = sp->x > x0 ? sp->x : x0; /* overlap of sprite and segment, screen x */
        int b = (sp->x + TILE < x0 + n) ? sp->x + TILE : x0 + n;
        uint8_t bank = (uint8_t)((sp->attr & HALTZ_GPU_SPR_BANK_MASK) << 4);
        bool hflip = (sp->attr & HALTZ_GPU_SPR_HFLIP) != 0;
        for (int sx = a; sx < b; sx++) {
            int px = sx - sp->x;
            uint8_t p = pix[hflip ? TILE - 1 - px : px];
            if (p != 0) {
                idx[sx - x0] = bank | p;
            }
        }
    }
}

/* ---- text -------------------------------------------------------------------------------- */

static void HOT draw_text(const gpu_state_t *s, int x0, int n, int y, bool blink_on,
                          uint8_t *idx) {
    int row = y / GLYPH;
    int gy = y % GLYPH;
    uint8_t shape = (uint8_t)(s->cursor_style & ~HALTZ_GPU_CURSOR_BLINK);
    int cursor_col = -1;
    if (shape != HALTZ_GPU_CURSOR_OFF && s->cursor_row == row &&
        (!(s->cursor_style & HALTZ_GPU_CURSOR_BLINK) || blink_on)) {
        cursor_col = s->cursor_col;
    }
    const uint8_t *chars = s->text_ch[row];
    const uint8_t *fgs = s->text_fg[row];
    const uint8_t *bgs = s->text_bg[row];
    int i = 0;
    while (i < n) {
        int x = x0 + i;
        int col = x / GLYPH;
        int gx = x % GLYPH;
        int run = GLYPH - gx; /* pixels left in this cell */
        if (run > n - i) {
            run = n - i;
        }
        uint8_t bits = s->font[chars[col]][gy];
        uint8_t fg = fgs[col];
        uint8_t bg = bgs[col];
        if (col == cursor_col) {
            if (shape == HALTZ_GPU_CURSOR_BLOCK) {
                uint8_t t = fg; /* inverse cell */
                fg = bg;
                bg = t;
            } else if (gy == GLYPH - 1) {
                bits = 0xFF; /* underline: the bottom row in the foreground colour */
            }
        }
        uint8_t mask = (uint8_t)(0x80u >> gx);
        uint8_t run_bits = (uint8_t)(bits & (uint8_t)(0xFFu >> gx) &
                                     (uint8_t)(0xFFu << (GLYPH - gx - run)));
        bool skip = bg == HALTZ_GPU_TRANSPARENT && (fg == HALTZ_GPU_TRANSPARENT || !run_bits);
        if (!skip) {
            uint8_t *out = &idx[i];
            for (int k = 0; k < run; k++, mask >>= 1) {
                uint8_t c = (bits & mask) ? fg : bg;
                if (c != HALTZ_GPU_TRANSPARENT) {
                    out[k] = c;
                }
            }
        }
        i += run;
    }
}

/* ---- compose ----------------------------------------------------------------------------- */

void HOT gpu_render_rect(const gpu_state_t *s, int x, int y, int w, int h, bool blink_on,
                         uint16_t *out) {
    uint8_t idx[W];
    sprite_list_t below, above;
    bool sprites = (s->layers & HALTZ_GPU_LAYER_SPRITES) != 0;
    if (sprites) {
        find_sprites(s, x, y, w, h, false, &below);
        find_sprites(s, x, y, w, h, true, &above);
    }
    const uint16_t *pal = s->palette;
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
        if (sprites && below.n) {
            draw_sprites(s, &below, x, w, sy, idx);
        }
        if (s->layers & HALTZ_GPU_LAYER_TEXT) {
            draw_text(s, x, w, sy, blink_on, idx);
        }
        if (sprites && above.n) {
            draw_sprites(s, &above, x, w, sy, idx);
        }
        uint16_t *o = &out[r * w];
        for (int i = 0; i < w; i++) {
            o[i] = pal[idx[i]]; /* index 0 = palette[0] = backdrop */
        }
    }
}
