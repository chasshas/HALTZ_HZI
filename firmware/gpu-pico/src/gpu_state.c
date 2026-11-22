#include "gpu_state.h"

#include <string.h>

#define W   HALTZ_GPU_SCREEN_W
#define H   HALTZ_GPU_SCREEN_H
#define COLS HALTZ_GPU_TEXT_COLS
#define ROWS HALTZ_GPU_TEXT_ROWS
#define TILE HALTZ_GPU_PATTERN_SIZE
#define MAP_MASK (HALTZ_GPU_MAP_PX - 1) /* 511 */

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int mini(int a, int b) {
    return a < b ? a : b;
}

/* ---- dirty cells ------------------------------------------------------------------------- */

#define ROW_ALL ((1ull << GPU_CELLS_X) - 1)

void gpu_state_mark(gpu_state_t *s, int x, int y, int w, int h) {
    int x0 = clampi(x, 0, W);
    int y0 = clampi(y, 0, H);
    int x1 = clampi(x + w, 0, W); /* exclusive */
    int y1 = clampi(y + h, 0, H);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    int c0 = x0 / GPU_CELL;
    int c1 = (x1 - 1) / GPU_CELL;
    uint64_t bits = ((c1 - c0 + 1) >= 64 ? ~0ull : ((1ull << (c1 - c0 + 1)) - 1)) << c0;
    for (int r = y0 / GPU_CELL; r <= (y1 - 1) / GPU_CELL; r++) {
        s->dirty[r] |= bits;
    }
}

void gpu_state_mark_all(gpu_state_t *s) {
    for (int r = 0; r < GPU_CELLS_Y; r++) {
        s->dirty[r] = ROW_ALL;
    }
}

bool gpu_state_is_dirty(const gpu_state_t *s) {
    for (int r = 0; r < GPU_CELLS_Y; r++) {
        if (s->dirty[r]) {
            return true;
        }
    }
    return false;
}

static void mark_cell(gpu_state_t *s, int col, int row) {
    if (col >= 0 && col < COLS && row >= 0 && row < ROWS) {
        s->dirty[row] |= 1ull << col;
    }
}

static int popcount64(uint64_t v) {
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

int gpu_state_take_dirty(gpu_state_t *s, gpu_rect_t *rects, int max) {
    int cells = 0;
    for (int r = 0; r < GPU_CELLS_Y; r++) {
        cells += popcount64(s->dirty[r]);
    }
    if (cells == 0 || max <= 0) {
        return 0;
    }

    int n = 0;
    bool full = cells * 4 >= GPU_CELLS_X * GPU_CELLS_Y * 3; /* >= 75 %: one big rectangle */
    for (int r = 0; r < GPU_CELLS_Y && !full; r++) {
        while (s->dirty[r] != 0) {
            /* Leftmost run of dirty cells in this row ... */
            int c0 = 0;
            while (!((s->dirty[r] >> c0) & 1u)) {
                c0++;
            }
            int c1 = c0;
            while (c1 + 1 < GPU_CELLS_X && ((s->dirty[r] >> (c1 + 1)) & 1u)) {
                c1++;
            }
            uint64_t run = (((1ull << (c1 - c0 + 1)) - 1)) << c0;
            /* ... grown downwards while the rows below have the same cells dirty. */
            int r1 = r;
            while (r1 + 1 < GPU_CELLS_Y && (s->dirty[r1 + 1] & run) == run) {
                r1++;
            }
            if (n == max) {
                full = true;
                break;
            }
            for (int rr = r; rr <= r1; rr++) {
                s->dirty[rr] &= ~run;
            }
            rects[n].x = (int16_t)(c0 * GPU_CELL);
            rects[n].y = (int16_t)(r * GPU_CELL);
            rects[n].w = (int16_t)((c1 - c0 + 1) * GPU_CELL);
            rects[n].h = (int16_t)((r1 - r + 1) * GPU_CELL);
            n++;
        }
    }
    if (full) {
        rects[0] = (gpu_rect_t){0, 0, W, H};
        n = 1;
    }
    memset(s->dirty, 0, sizeof(s->dirty));
    return n;
}

/* ---- cursor ------------------------------------------------------------------------------ */

bool gpu_state_cursor_rect(const gpu_state_t *s, gpu_rect_t *r) {
    if ((s->cursor_style & ~HALTZ_GPU_CURSOR_BLINK) == HALTZ_GPU_CURSOR_OFF ||
        s->cursor_col >= COLS || s->cursor_row >= ROWS) {
        return false;
    }
    *r = (gpu_rect_t){(int16_t)(s->cursor_col * GPU_CELL), (int16_t)(s->cursor_row * GPU_CELL),
                      GPU_CELL, GPU_CELL};
    return true;
}

bool gpu_state_cursor_blinks(const gpu_state_t *s) {
    gpu_rect_t r;
    return (s->cursor_style & HALTZ_GPU_CURSOR_BLINK) && gpu_state_cursor_rect(s, &r);
}

/* ---- init -------------------------------------------------------------------------------- */

void gpu_state_init(gpu_state_t *s) {
    memset(s, 0, sizeof(*s)); /* tiles, patterns, sprites (hidden), bitmap, scroll: 0 */
    memcpy(s->palette, haltz_gpu_default_palette, sizeof(s->palette));
    memcpy(s->font, haltz_gpu_default_font, sizeof(s->font));
    s->layers = HALTZ_GPU_LAYERS_DEFAULT;
    memset(s->text_ch, ' ', sizeof(s->text_ch));
    memset(s->text_fg, HALTZ_COLOR_WHITE, sizeof(s->text_fg));
    s->cursor_style = HALTZ_GPU_CURSOR_OFF;
    gpu_state_mark_all(s);
}

/* ---- text -------------------------------------------------------------------------------- */

static void text_set(gpu_state_t *s, int col, int row, uint8_t ch, uint8_t fg, uint8_t bg) {
    s->text_ch[row][col] = ch;
    s->text_fg[row][col] = fg;
    s->text_bg[row][col] = bg;
    mark_cell(s, col, row);
}

static void text_put(gpu_state_t *s, const haltz_gpu_text_put_t *m) {
    int pos = m->row * COLS + m->col;
    for (int i = 0; i < m->count && pos < COLS * ROWS; i++, pos++) {
        text_set(s, pos % COLS, pos / COLS, m->chars[i], m->fg, m->bg);
    }
}

static void text_fill_rows(gpu_state_t *s, int col0, int col1, int row0, int row1, uint8_t ch,
                           uint8_t fg, uint8_t bg) {
    for (int r = row0; r < row1; r++) {
        for (int c = col0; c < col1; c++) {
            text_set(s, c, r, ch, fg, bg);
        }
    }
}

static void text_fill(gpu_state_t *s, const haltz_gpu_text_fill_t *m) {
    text_fill_rows(s, m->col, mini(m->col + m->w, COLS), m->row, mini(m->row + m->h, ROWS),
                   m->ch, m->fg, m->bg);
}

static void text_copy_row(gpu_state_t *s, int dst, int src) {
    memcpy(s->text_ch[dst], s->text_ch[src], COLS);
    memcpy(s->text_fg[dst], s->text_fg[src], COLS);
    memcpy(s->text_bg[dst], s->text_bg[src], COLS);
}

static void text_scroll(gpu_state_t *s, const haltz_gpu_text_scroll_t *m) {
    int top = m->top;
    int bottom = mini(m->top + m->height, ROWS); /* exclusive */
    if (top >= bottom) {
        return;
    }
    int h = bottom - top;
    int n = m->rows;
    if (n >= h || -n >= h) {
        text_fill_rows(s, 0, COLS, top, bottom, m->ch, m->fg, m->bg);
        return;
    }
    if (n > 0) { /* content moves up */
        for (int r = top; r < bottom - n; r++) {
            text_copy_row(s, r, r + n);
        }
        text_fill_rows(s, 0, COLS, bottom - n, bottom, m->ch, m->fg, m->bg);
    } else if (n < 0) { /* content moves down */
        int k = -n;
        for (int r = bottom - 1; r >= top + k; r--) {
            text_copy_row(s, r, r - k);
        }
        text_fill_rows(s, 0, COLS, top, top + k, m->ch, m->fg, m->bg);
    }
    gpu_state_mark(s, 0, top * GPU_CELL, W, h * GPU_CELL);
}

static void text_cursor(gpu_state_t *s, const haltz_gpu_text_cursor_t *m) {
    mark_cell(s, s->cursor_col, s->cursor_row);
    s->cursor_col = m->col;
    s->cursor_row = m->row;
    s->cursor_style = m->style;
    mark_cell(s, s->cursor_col, s->cursor_row);
}

/* ---- tiles and sprites ------------------------------------------------------------------- */

/* Mark where map cell (mx, my) is on screen at the current scroll. */
static void mark_tile(gpu_state_t *s, int mx, int my) {
    int x = (mx * TILE - s->scroll_x) & MAP_MASK;
    int y = (my * TILE - s->scroll_y) & MAP_MASK;
    if (x > HALTZ_GPU_MAP_PX - TILE) {
        x -= HALTZ_GPU_MAP_PX; /* wraps in from the left edge */
    }
    if (y > HALTZ_GPU_MAP_PX - TILE) {
        y -= HALTZ_GPU_MAP_PX;
    }
    gpu_state_mark(s, x, y, TILE, TILE);
}

static void tile_put(gpu_state_t *s, const haltz_gpu_tile_put_t *m) {
    int mx = m->x;
    int my = m->y;
    for (int i = 0; i < m->count && my < HALTZ_GPU_MAP_H; i++) {
        s->map[my][mx] = m->entries[i];
        mark_tile(s, mx, my);
        if (++mx == HALTZ_GPU_MAP_W) {
            mx = 0;
            my++;
        }
    }
}

static void tile_fill(gpu_state_t *s, const haltz_gpu_tile_fill_t *m) {
    for (int my = m->y; my < mini(m->y + m->h, HALTZ_GPU_MAP_H); my++) {
        for (int mx = m->x; mx < mini(m->x + m->w, HALTZ_GPU_MAP_W); mx++) {
            s->map[my][mx] = m->entry;
            mark_tile(s, mx, my);
        }
    }
}

static void mark_sprite(gpu_state_t *s, const gpu_sprite_t *sp) {
    if (sp->attr & HALTZ_GPU_SPR_VISIBLE) {
        gpu_state_mark(s, sp->x, sp->y, TILE, TILE);
    }
}

static void sprite_set(gpu_state_t *s, const haltz_gpu_sprite_set_t *m) {
    gpu_sprite_t *sp = &s->sprites[m->id];
    mark_sprite(s, sp);
    sp->x = m->x;
    sp->y = m->y;
    sp->pattern = m->pattern;
    sp->attr = m->attr;
    mark_sprite(s, sp);
}

/* ---- bitmap ------------------------------------------------------------------------------ */

static void bmp_pixels(gpu_state_t *s, const haltz_gpu_bmp_pixels_t *m) {
    if (m->x >= W || m->y >= H) {
        return;
    }
    int n = mini(m->count, W - m->x);
    memcpy(&s->bitmap[m->y][m->x], m->pixels, (size_t)n);
    gpu_state_mark(s, m->x, m->y, n, 1);
}

static void bmp_fill(gpu_state_t *s, const haltz_gpu_bmp_fill_t *m) {
    int x1 = mini(m->x + m->w, W);
    int y1 = mini(m->y + m->h, H);
    if (m->x >= x1 || m->y >= y1) {
        return;
    }
    for (int y = m->y; y < y1; y++) {
        memset(&s->bitmap[y][m->x], m->color, (size_t)(x1 - m->x));
    }
    gpu_state_mark(s, m->x, m->y, x1 - m->x, y1 - m->y);
}

static void bmp_line(gpu_state_t *s, const haltz_gpu_bmp_line_t *m) {
    /* Bresenham, every point checked against the screen (lines are short in practice;
     * the longest possible line is 65536 steps). */
    int32_t x = m->x0, y = m->y0;
    int32_t dx = m->x1 > m->x0 ? m->x1 - m->x0 : m->x0 - m->x1;
    int32_t dy = m->y1 > m->y0 ? m->y0 - m->y1 : m->y1 - m->y0; /* <= 0 */
    int32_t stepx = m->x0 < m->x1 ? 1 : -1;
    int32_t stepy = m->y0 < m->y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        if (x >= 0 && x < W && y >= 0 && y < H) {
            s->bitmap[y][x] = m->color;
        }
        if (x == m->x1 && y == m->y1) {
            break;
        }
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += stepx;
        }
        if (e2 <= dx) {
            err += dx;
            y += stepy;
        }
    }
    int x0 = m->x0 < m->x1 ? m->x0 : m->x1;
    int y0 = m->y0 < m->y1 ? m->y0 : m->y1;
    gpu_state_mark(s, x0, y0, (int)dx + 1, (int)-dy + 1);
}

static void bmp_copy(gpu_state_t *s, const haltz_gpu_bmp_copy_t *m) {
    /* Copy only pixels whose source and destination are both on screen. */
    int c0 = 0, c1 = m->w; /* columns of the rectangle, [c0, c1) */
    int r0 = 0, r1 = m->h;
    c0 = clampi(c0, -m->sx, c1);
    c0 = clampi(c0, -m->dx, c1);
    c1 = mini(c1, W - m->sx);
    c1 = mini(c1, W - m->dx);
    r0 = clampi(r0, -m->sy, r1);
    r0 = clampi(r0, -m->dy, r1);
    r1 = mini(r1, H - m->sy);
    r1 = mini(r1, H - m->dy);
    if (c1 <= c0 || r1 <= r0) {
        return;
    }
    size_t n = (size_t)(c1 - c0);
    if (m->dy > m->sy) { /* moving down: copy bottom row first */
        for (int r = r1 - 1; r >= r0; r--) {
            memmove(&s->bitmap[m->dy + r][m->dx + c0], &s->bitmap[m->sy + r][m->sx + c0], n);
        }
    } else {
        for (int r = r0; r < r1; r++) {
            memmove(&s->bitmap[m->dy + r][m->dx + c0], &s->bitmap[m->sy + r][m->sx + c0], n);
        }
    }
    gpu_state_mark(s, m->dx + c0, m->dy + r0, c1 - c0, r1 - r0);
}

/* ---- dispatch ---------------------------------------------------------------------------- */

bool gpu_state_apply(gpu_state_t *s, const haltz_gpu_cmd_t *cmd) {
    switch (cmd->type) {
    case HALTZ_MSG_GPU_INIT:
        gpu_state_init(s);
        return true;
    case HALTZ_MSG_GPU_LAYERS:
        s->layers = cmd->u.layers.mask;
        gpu_state_mark_all(s);
        return true;
    case HALTZ_MSG_GPU_PALETTE:
        memcpy(&s->palette[cmd->u.palette.first], cmd->u.palette.colors,
               cmd->u.palette.count * sizeof(uint16_t));
        gpu_state_mark_all(s);
        return true;
    case HALTZ_MSG_GPU_TEXT_PUT:
        text_put(s, &cmd->u.text_put);
        return true;
    case HALTZ_MSG_GPU_TEXT_FILL:
        text_fill(s, &cmd->u.text_fill);
        return true;
    case HALTZ_MSG_GPU_TEXT_SCROLL:
        text_scroll(s, &cmd->u.text_scroll);
        return true;
    case HALTZ_MSG_GPU_TEXT_CURSOR:
        text_cursor(s, &cmd->u.text_cursor);
        return true;
    case HALTZ_MSG_GPU_FONT_LOAD:
        memcpy(s->font[cmd->u.font_load.first], cmd->u.font_load.glyphs,
               (size_t)cmd->u.font_load.count * HALTZ_GPU_GLYPH_BYTES);
        gpu_state_mark_all(s);
        return true;
    case HALTZ_MSG_GPU_PATTERN_LOAD: {
        const haltz_gpu_pattern_load_t *m = &cmd->u.pattern_load;
        memcpy(s->patterns[m->index], m->data, HALTZ_GPU_PATTERN_BYTES);
        uint16_t rows = 0;
        for (int r = 0; r < TILE; r++) {
            for (int b = 0; b < TILE / 2; b++) {
                if (m->data[r * (TILE / 2) + b] != 0) {
                    rows |= (uint16_t)(1u << r);
                    break;
                }
            }
        }
        s->pattern_rows[m->index] = rows;
        gpu_state_mark_all(s);
        return true;
    }
    case HALTZ_MSG_GPU_TILE_PUT:
        tile_put(s, &cmd->u.tile_put);
        return true;
    case HALTZ_MSG_GPU_TILE_FILL:
        tile_fill(s, &cmd->u.tile_fill);
        return true;
    case HALTZ_MSG_GPU_TILE_SCROLL:
        s->scroll_x = cmd->u.tile_scroll.sx & MAP_MASK;
        s->scroll_y = cmd->u.tile_scroll.sy & MAP_MASK;
        gpu_state_mark_all(s);
        return true;
    case HALTZ_MSG_GPU_SPRITE_SET:
        sprite_set(s, &cmd->u.sprite_set);
        return true;
    case HALTZ_MSG_GPU_BMP_PIXELS:
        bmp_pixels(s, &cmd->u.bmp_pixels);
        return true;
    case HALTZ_MSG_GPU_BMP_FILL:
        bmp_fill(s, &cmd->u.bmp_fill);
        return true;
    case HALTZ_MSG_GPU_BMP_LINE:
        bmp_line(s, &cmd->u.bmp_line);
        return true;
    case HALTZ_MSG_GPU_BMP_COPY:
        bmp_copy(s, &cmd->u.bmp_copy);
        return true;
    default: /* PRESENT, STATUS_REQ, BACKLIGHT: not picture state */
        return false;
    }
}
