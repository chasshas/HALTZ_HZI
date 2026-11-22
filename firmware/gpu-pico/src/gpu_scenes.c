#include "gpu_scenes.h"

#include <stdio.h>
#include <string.h>

/* Every change goes through gpu_state_apply(), like a command from the S3. */
static haltz_gpu_cmd_t g_cmd;

static void apply(gpu_state_t *s) {
    gpu_state_apply(s, &g_cmd);
}

static void text(gpu_state_t *s, int col, int row, uint8_t fg, uint8_t bg, const char *str) {
    g_cmd.type = HALTZ_MSG_GPU_TEXT_PUT;
    haltz_gpu_text_put_t *m = &g_cmd.u.text_put;
    size_t n = strlen(str);
    m->col = (uint8_t)col;
    m->row = (uint8_t)row;
    m->fg = fg;
    m->bg = bg;
    m->count = (uint8_t)(n > HALTZ_GPU_TEXT_PUT_MAX ? HALTZ_GPU_TEXT_PUT_MAX : n);
    memcpy(m->chars, str, m->count);
    apply(s);
}

static void glyph(gpu_state_t *s, int col, int row, uint8_t fg, uint8_t bg, uint8_t ch) {
    char str[2] = {(char)ch, 0};
    text(s, col, row, fg, bg, str);
}

/* Box with box-drawing glyphs, cells (c0, r0) .. (c1, r1) inclusive. */
static void box(gpu_state_t *s, int c0, int r0, int c1, int r1, uint8_t fg) {
    for (int c = c0 + 1; c < c1; c++) {
        glyph(s, c, r0, fg, 0, HALTZ_GLYPH_BOX_H);
        glyph(s, c, r1, fg, 0, HALTZ_GLYPH_BOX_H);
    }
    for (int r = r0 + 1; r < r1; r++) {
        glyph(s, c0, r, fg, 0, HALTZ_GLYPH_BOX_V);
        glyph(s, c1, r, fg, 0, HALTZ_GLYPH_BOX_V);
    }
    glyph(s, c0, r0, fg, 0, HALTZ_GLYPH_BOX_DOWN_RIGHT);
    glyph(s, c1, r0, fg, 0, HALTZ_GLYPH_BOX_DOWN_LEFT);
    glyph(s, c0, r1, fg, 0, HALTZ_GLYPH_BOX_UP_RIGHT);
    glyph(s, c1, r1, fg, 0, HALTZ_GLYPH_BOX_UP_LEFT);
}

/* ---- boot screen ------------------------------------------------------------------------- */

void gpu_scene_boot(gpu_state_t *s, unsigned proto_version, const char *git_hash) {
    char line[48];
    gpu_state_init(s);
    box(s, 1, 1, 38, 6, HALTZ_COLOR_LIGHT_BLUE);
    text(s, 3, 2, HALTZ_COLOR_WHITE, 0, "HALTZ GPU");
    snprintf(line, sizeof(line), "protocol v%u   git %.8s", proto_version, git_hash);
    text(s, 3, 3, HALTZ_COLOR_LIGHT_GREY, 0, line);
    text(s, 3, 4, HALTZ_COLOR_LIGHT_GREY, 0, "320x240  text 40x30  64 sprites");

    /* Colour check: the 15 system colours as blocks (wrong colours = TFT settings). */
    text(s, 3, 8, HALTZ_COLOR_LIGHT_GREY, 0, "colours");
    for (int i = 1; i <= 15; i++) {
        glyph(s, 10 + 2 * (i - 1), 8, (uint8_t)i, 0, HALTZ_GLYPH_BLOCK_FULL);
    }
    text(s, 3, 10, HALTZ_COLOR_LIGHT_GREY, 0, "shades");
    glyph(s, 10, 10, HALTZ_COLOR_WHITE, 0, HALTZ_GLYPH_SHADE_LIGHT);
    glyph(s, 12, 10, HALTZ_COLOR_WHITE, 0, HALTZ_GLYPH_SHADE_MEDIUM);
    glyph(s, 14, 10, HALTZ_COLOR_WHITE, 0, HALTZ_GLYPH_SHADE_DARK);
    glyph(s, 16, 10, HALTZ_COLOR_WHITE, 0, HALTZ_GLYPH_BLOCK_FULL);

    text(s, 3, 14, HALTZ_COLOR_YELLOW, 0, "waiting for CPU...");
    text(s, 3, 27, HALTZ_COLOR_DARK_GREY, 0, "USB console: d = demo, s = stats");
}

/* ---- demo -------------------------------------------------------------------------------- */

enum { PAT_BRICK = 1, PAT_BALL = 2, PAT_ARROW = 3 };
enum { DEMO_BALLS = 8, DEMO_CURSOR = HALTZ_GPU_SPRITE_CURSOR };

/* 16 x 16 pattern from 16 strings of 16 hex digits ('.' = 0). */
static void load_pattern(gpu_state_t *s, uint8_t index, const char *const rows[16]) {
    g_cmd.type = HALTZ_MSG_GPU_PATTERN_LOAD;
    haltz_gpu_pattern_load_t *m = &g_cmd.u.pattern_load;
    m->index = index;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 2) {
            uint8_t v[2];
            for (int k = 0; k < 2; k++) {
                char c = rows[y][x + k];
                v[k] = (uint8_t)(c == '.' ? 0 : (c <= '9' ? c - '0' : c - 'A' + 10));
            }
            m->data[y * 8 + x / 2] = (uint8_t)(v[0] << 4 | v[1]);
        }
    }
    apply(s);
}

/* 9 = light grey mortar, B = brown brick */
static const char *const brick[16] = {
    "9999999999999999", "BBBBBBB9BBBBBBB9", "BBBBBBB9BBBBBBB9", "BBBBBBB9BBBBBBB9",
    "BBBBBBB9BBBBBBB9", "BBBBBBB9BBBBBBB9", "BBBBBBB9BBBBBBB9", "BBBBBBB9BBBBBBB9",
    "9999999999999999", "BBB9BBBBBBB9BBBB", "BBB9BBBBBBB9BBBB", "BBB9BBBBBBB9BBBB",
    "BBB9BBBBBBB9BBBB", "BBB9BBBBBBB9BBBB", "BBB9BBBBBBB9BBBB", "BBB9BBBBBBB9BBBB",
};
/* 2 = colour of the bank (red in bank 0), 1 = white highlight */
static const char *const ball[16] = {
    "................", ".....222222.....", "...2222222222...", "..222222222222..",
    "..221122222222..", ".22111122222222.", ".22111222222222.", ".22212222222222.",
    ".22222222222222.", ".22222222222222.", ".22222222222222.", "..222222222222..",
    "..222222222222..", "...2222222222...", ".....222222.....", "................",
};
/* 1 = white, 8 = dark grey outline */
static const char *const arrow[16] = {
    "8...............", "88..............", "818.............", "8118............",
    "81118...........", "811118..........", "8111118.........", "81111118........",
    "811111118.......", "8111111118......", "81111188888.....", "8118118.........",
    "818.8118........", "88..8118........", "8....8118.......", ".....888........",
};

static uint32_t tri(uint32_t v, uint32_t max) { /* triangle wave 0..max..0 */
    v %= 2 * max;
    return v <= max ? v : 2 * max - v;
}

void gpu_scene_demo_init(gpu_state_t *s) {
    gpu_state_init(s);

    g_cmd.type = HALTZ_MSG_GPU_LAYERS;
    g_cmd.u.layers.mask = HALTZ_GPU_LAYERS_ALL;
    apply(s);

    /* Bitmap: dark blue-grey bands, then a fan of lines in the system colours. */
    for (int band = 0; band < 10; band++) {
        g_cmd.type = HALTZ_MSG_GPU_BMP_FILL;
        g_cmd.u.bmp_fill = (haltz_gpu_bmp_fill_t){0, (uint16_t)(band * 24), HALTZ_GPU_SCREEN_W,
                                                  24, (uint8_t)(HALTZ_COLOR_CUBE_FIRST + band / 2)};
        apply(s);
    }
    for (int i = 0; i <= 32; i++) {
        g_cmd.type = HALTZ_MSG_GPU_BMP_LINE;
        g_cmd.u.bmp_line = (haltz_gpu_bmp_line_t){160, 120, (int16_t)(i * 10), 0,
                                                  (uint8_t)(1 + i % 15)};
        apply(s);
    }

    load_pattern(s, PAT_BRICK, brick);
    load_pattern(s, PAT_BALL, ball);
    load_pattern(s, PAT_ARROW, arrow);

    /* Tiles: a brick floor (map rows 13-14 = bottom of the screen) and some pillars. */
    g_cmd.type = HALTZ_MSG_GPU_TILE_FILL;
    g_cmd.u.tile_fill = (haltz_gpu_tile_fill_t){0, 13, HALTZ_GPU_MAP_W, 2,
                                                HALTZ_GPU_TILE(PAT_BRICK, 0, 0)};
    apply(s);
    for (int x = 3; x < HALTZ_GPU_MAP_W; x += 7) {
        g_cmd.u.tile_fill = (haltz_gpu_tile_fill_t){(uint8_t)x, 11, 1, 2,
                                                    HALTZ_GPU_TILE(PAT_BRICK, 0, 0)};
        apply(s);
    }

    text(s, 1, 1, HALTZ_COLOR_WHITE, HALTZ_COLOR_DARK_BLUE, " HALTZ GPU demo ");
    text(s, 1, 3, HALTZ_COLOR_YELLOW, 0, "bitmap + tiles + sprites + text");
    text(s, 1, 4, HALTZ_COLOR_LIGHT_GREY, 0, "any key on the console stops it");

    g_cmd.type = HALTZ_MSG_GPU_TEXT_CURSOR;
    g_cmd.u.text_cursor = (haltz_gpu_text_cursor_t){
        17, 1, HALTZ_GPU_CURSOR_BLOCK | HALTZ_GPU_CURSOR_BLINK};
    apply(s);

    gpu_scene_demo_step(s, 0);
}

void gpu_scene_demo_step(gpu_state_t *s, uint32_t frame) {
    g_cmd.type = HALTZ_MSG_GPU_TILE_SCROLL;
    g_cmd.u.tile_scroll = (haltz_gpu_tile_scroll_t){(uint16_t)(frame * 2), 0};
    apply(s);

    /* Balls in 4 palette banks: pixel value 2 is red in bank 0 and a cube colour (green,
     * teal, purple) in banks 2, 3, 6. Same pattern, different colours. */
    static const uint8_t banks[4] = {0, 2, 3, 6};
    g_cmd.type = HALTZ_MSG_GPU_SPRITE_SET;
    for (uint32_t i = 0; i < DEMO_BALLS; i++) {
        haltz_gpu_sprite_set_t *m = &g_cmd.u.sprite_set;
        m->id = (uint8_t)i;
        m->x = (int16_t)tri(frame * (2 + i % 3) + i * 37, HALTZ_GPU_SCREEN_W - 16);
        m->y = (int16_t)tri(frame * (1 + i % 4) + i * 53, 176 - 16);
        m->pattern = PAT_BALL;
        uint8_t flip = (i & 4) ? HALTZ_GPU_SPR_HFLIP : 0;
        m->attr = (uint8_t)(HALTZ_GPU_SPR_VISIBLE | banks[i % 4] | flip);
        apply(s);
    }
    /* Pointer: drawn above the text. */
    haltz_gpu_sprite_set_t *c = &g_cmd.u.sprite_set;
    c->id = DEMO_CURSOR;
    c->x = (int16_t)(40 + tri(frame * 3, 200));
    c->y = (int16_t)(4 + tri(frame * 2, 60));
    c->pattern = PAT_ARROW;
    c->attr = HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_ABOVE_TEXT;
    apply(s);

    char line[24];
    snprintf(line, sizeof(line), "frame %6lu", (unsigned long)frame);
    text(s, 27, 28, HALTZ_COLOR_WHITE, HALTZ_COLOR_BLACK, line);
}
