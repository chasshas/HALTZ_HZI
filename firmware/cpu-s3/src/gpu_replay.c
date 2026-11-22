#include "gpu_replay.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    haltz_gpu_cmd_t *c;
    gpu_replay_emit_fn emit;
    void *ctx;
} out_t;

static int mini(int a, int b) {
    return a < b ? a : b;
}

static void put(const out_t *o) {
    o->emit(o->c, o->ctx);
}

static bool text_cell_default(const gpu_state_t *s, int r, int c) {
    return s->text_ch[r][c] == ' ' && s->text_fg[r][c] == HALTZ_COLOR_WHITE &&
           s->text_bg[r][c] == 0;
}

static void replay_palette_font_layers(const gpu_state_t *s, const out_t *o) {
    haltz_gpu_cmd_t *c = o->c;
    if (memcmp(s->palette, haltz_gpu_default_palette, sizeof(s->palette)) != 0) {
        for (int first = 0; first < HALTZ_GPU_PALETTE_SIZE; first += HALTZ_GPU_PALETTE_MAX) {
            int n = mini(HALTZ_GPU_PALETTE_MAX, HALTZ_GPU_PALETTE_SIZE - first);
            c->type = HALTZ_MSG_GPU_PALETTE;
            c->u.palette.first = (uint8_t)first;
            c->u.palette.count = (uint8_t)n;
            memcpy(c->u.palette.colors, &s->palette[first], (size_t)n * sizeof(uint16_t));
            put(o);
        }
    }
    for (int first = 0; first < HALTZ_GPU_FONT_GLYPHS; first += HALTZ_GPU_FONT_LOAD_MAX) {
        int n = mini(HALTZ_GPU_FONT_LOAD_MAX, HALTZ_GPU_FONT_GLYPHS - first);
        size_t bytes = (size_t)n * HALTZ_GPU_GLYPH_BYTES;
        if (memcmp(s->font[first], haltz_gpu_default_font[first], bytes) != 0) {
            c->type = HALTZ_MSG_GPU_FONT_LOAD;
            c->u.font_load.first = (uint8_t)first;
            c->u.font_load.count = (uint8_t)n;
            memcpy(c->u.font_load.glyphs, s->font[first], bytes);
            put(o);
        }
    }
    if (s->layers != HALTZ_GPU_LAYERS_DEFAULT) {
        c->type = HALTZ_MSG_GPU_LAYERS;
        c->u.layers.mask = s->layers;
        put(o);
    }
}

static void replay_text(const gpu_state_t *s, const out_t *o) {
    haltz_gpu_cmd_t *c = o->c;
    for (int r = 0; r < HALTZ_GPU_TEXT_ROWS; r++) {
        int col = 0;
        while (col < HALTZ_GPU_TEXT_COLS) {
            if (text_cell_default(s, r, col)) {
                col++;
                continue;
            }
            /* A run of cells with the same colours, without default cells at its end. */
            int c0 = col, last = col;
            uint8_t fg = s->text_fg[r][c0], bg = s->text_bg[r][c0];
            while (col < HALTZ_GPU_TEXT_COLS && s->text_fg[r][col] == fg &&
                   s->text_bg[r][col] == bg) {
                if (!text_cell_default(s, r, col)) {
                    last = col;
                }
                col++;
            }
            c->type = HALTZ_MSG_GPU_TEXT_PUT;
            haltz_gpu_text_put_t *m = &c->u.text_put;
            m->col = (uint8_t)c0;
            m->row = (uint8_t)r;
            m->fg = fg;
            m->bg = bg;
            m->count = (uint8_t)(last - c0 + 1);
            memcpy(m->chars, &s->text_ch[r][c0], m->count);
            put(o);
        }
    }
    if (s->cursor_style != HALTZ_GPU_CURSOR_OFF || s->cursor_col != 0 || s->cursor_row != 0) {
        c->type = HALTZ_MSG_GPU_TEXT_CURSOR;
        c->u.text_cursor =
            (haltz_gpu_text_cursor_t){s->cursor_col, s->cursor_row, s->cursor_style};
        put(o);
    }
}

static void replay_tiles_sprites(const gpu_state_t *s, const out_t *o) {
    haltz_gpu_cmd_t *c = o->c;
    for (int i = 0; i < HALTZ_GPU_PATTERNS; i++) {
        const uint8_t *d = s->patterns[i];
        bool empty = true;
        for (int k = 0; k < HALTZ_GPU_PATTERN_BYTES && empty; k++) {
            empty = d[k] == 0;
        }
        if (!empty) {
            c->type = HALTZ_MSG_GPU_PATTERN_LOAD;
            c->u.pattern_load.index = (uint8_t)i;
            memcpy(c->u.pattern_load.data, d, HALTZ_GPU_PATTERN_BYTES);
            put(o);
        }
    }
    for (int y = 0; y < HALTZ_GPU_MAP_H; y++) {
        int x0 = 0, x1 = HALTZ_GPU_MAP_W - 1;
        while (x0 < HALTZ_GPU_MAP_W && s->map[y][x0] == 0) {
            x0++;
        }
        if (x0 == HALTZ_GPU_MAP_W) {
            continue;
        }
        while (s->map[y][x1] == 0) {
            x1--;
        }
        c->type = HALTZ_MSG_GPU_TILE_PUT;
        c->u.tile_put.x = (uint8_t)x0;
        c->u.tile_put.y = (uint8_t)y;
        c->u.tile_put.count = (uint8_t)(x1 - x0 + 1);
        memcpy(c->u.tile_put.entries, &s->map[y][x0], (size_t)(x1 - x0 + 1) * sizeof(uint16_t));
        put(o);
    }
    if (s->scroll_x != 0 || s->scroll_y != 0) {
        c->type = HALTZ_MSG_GPU_TILE_SCROLL;
        c->u.tile_scroll = (haltz_gpu_tile_scroll_t){s->scroll_x, s->scroll_y};
        put(o);
    }
    for (int i = 0; i < HALTZ_GPU_SPRITES; i++) {
        const gpu_sprite_t *sp = &s->sprites[i];
        if (sp->x != 0 || sp->y != 0 || sp->pattern != 0 || sp->attr != 0) {
            c->type = HALTZ_MSG_GPU_SPRITE_SET;
            c->u.sprite_set =
                (haltz_gpu_sprite_set_t){(uint8_t)i, sp->x, sp->y, sp->pattern, sp->attr};
            put(o);
        }
    }
}

static void replay_bitmap(const gpu_state_t *s, const out_t *o) {
    haltz_gpu_cmd_t *c = o->c;
    /* Rows of one colour become BMP_FILLs, merged downwards; other rows BMP_PIXELS.
     * The loop runs one row past the end to flush the last pending fill. */
    haltz_gpu_bmp_fill_t fill = {0};
    for (int y = 0; y <= HALTZ_GPU_SCREEN_H; y++) {
        int x0 = 0, x1 = HALTZ_GPU_SCREEN_W - 1;
        bool uniform = false, empty = true;
        const uint8_t *row = NULL;
        if (y < HALTZ_GPU_SCREEN_H) {
            row = s->bitmap[y];
            while (x0 < HALTZ_GPU_SCREEN_W && row[x0] == 0) {
                x0++;
            }
            empty = x0 == HALTZ_GPU_SCREEN_W;
            if (!empty) {
                while (row[x1] == 0) {
                    x1--;
                }
                uniform = true;
                for (int x = x0; x <= x1 && uniform; x++) {
                    uniform = row[x] == row[x0];
                }
            }
        }
        if (uniform && fill.h > 0 && fill.x == x0 && fill.w == x1 - x0 + 1 &&
            fill.color == row[x0] && fill.y + fill.h == y) {
            fill.h++;
            continue;
        }
        if (fill.h > 0) {
            c->type = HALTZ_MSG_GPU_BMP_FILL;
            c->u.bmp_fill = fill;
            put(o);
            fill.h = 0;
        }
        if (empty) {
            continue;
        }
        if (uniform) {
            fill = (haltz_gpu_bmp_fill_t){(uint16_t)x0, (uint16_t)y, (uint16_t)(x1 - x0 + 1), 1,
                                          row[x0]};
            continue;
        }
        for (int x = x0; x <= x1; x += HALTZ_GPU_BMP_PIXELS_MAX) {
            c->type = HALTZ_MSG_GPU_BMP_PIXELS;
            c->u.bmp_pixels.x = (uint16_t)x;
            c->u.bmp_pixels.y = (uint16_t)y;
            c->u.bmp_pixels.count = (uint8_t)mini(HALTZ_GPU_BMP_PIXELS_MAX, x1 - x + 1);
            memcpy(c->u.bmp_pixels.pixels, &row[x], c->u.bmp_pixels.count);
            put(o);
        }
    }
}

void gpu_replay(const gpu_state_t *s, haltz_gpu_cmd_t *scratch, gpu_replay_emit_fn emit,
                void *ctx) {
    out_t o = {scratch, emit, ctx};
    replay_palette_font_layers(s, &o);
    replay_text(s, &o);
    replay_tiles_sprites(s, &o);
    replay_bitmap(s, &o);
}
