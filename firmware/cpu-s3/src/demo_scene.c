#include "demo_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define W        HALTZ_GPU_SCREEN_W
#define H        HALTZ_GPU_SCREEN_H
#define GROUND_Y 208 /* top of the grass (map row 13) */
#define BALLS    40

/* Colour cube index for levels r, g, b in 0..5 (protocol section 3). */
#define CUBE(r, g, b) ((uint8_t)(HALTZ_COLOR_CUBE_FIRST + 36 * (r) + 6 * (g) + (b)))

/* Stars use palette entries 248-255 (greys by default) and twinkle by rewriting them. */
#define STAR_FIRST 248
#define STAR_COLS  8

/* Frame-rate graph in the bitmap (inside, pixels), and how far it moves per sample. */
#define GRAPH_X    233
#define GRAPH_Y    15
#define GRAPH_W    81
#define GRAPH_H    30
#define GRAPH_STEP 3
#define GRAPH_MAX_FPS 30

enum { PAT_BALL = 1, PAT_RUN_A, PAT_RUN_B, PAT_UFO, PAT_ARROW, PAT_GRASS, PAT_DIRT, PAT_BUSH };
enum { SPR_RUNNER = BALLS, SPR_UFO, SPR_POINTER = HALTZ_GPU_SPRITE_CURSOR };
enum { GLYPH_HEART = 0xA0, GLYPH_NOTE, GLYPH_STAR, GLYPH_SMILE };

/* ---- output ------------------------------------------------------------------------------ */

static haltz_gpu_cmd_t g_cmd;
static demo_emit_fn g_emit;
static void *g_ctx;

static void send(void) {
    g_emit(&g_cmd, g_ctx);
}

static void text(int col, int row, uint8_t fg, uint8_t bg, const char *s) {
    g_cmd.type = HALTZ_MSG_GPU_TEXT_PUT;
    haltz_gpu_text_put_t *m = &g_cmd.u.text_put;
    size_t n = strlen(s);
    m->col = (uint8_t)col;
    m->row = (uint8_t)row;
    m->fg = fg;
    m->bg = bg;
    m->count = (uint8_t)(n > HALTZ_GPU_TEXT_PUT_MAX ? HALTZ_GPU_TEXT_PUT_MAX : n);
    memcpy(m->chars, s, m->count);
    send();
}

static void fill(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0) {
        return;
    }
    g_cmd.type = HALTZ_MSG_GPU_BMP_FILL;
    g_cmd.u.bmp_fill = (haltz_gpu_bmp_fill_t){(uint16_t)x, (uint16_t)y, (uint16_t)w,
                                              (uint16_t)h, color};
    send();
}

static void line(int x0, int y0, int x1, int y1, uint8_t color) {
    g_cmd.type = HALTZ_MSG_GPU_BMP_LINE;
    g_cmd.u.bmp_line = (haltz_gpu_bmp_line_t){(int16_t)x0, (int16_t)y0, (int16_t)x1,
                                              (int16_t)y1, color};
    send();
}

/* One bitmap row, split into BMP_PIXELS messages. */
static void pixels_row(int y, const uint8_t *px) {
    for (int x = 0; x < W; x += HALTZ_GPU_BMP_PIXELS_MAX) {
        int n = W - x < HALTZ_GPU_BMP_PIXELS_MAX ? W - x : HALTZ_GPU_BMP_PIXELS_MAX;
        g_cmd.type = HALTZ_MSG_GPU_BMP_PIXELS;
        g_cmd.u.bmp_pixels.x = (uint16_t)x;
        g_cmd.u.bmp_pixels.y = (uint16_t)y;
        g_cmd.u.bmp_pixels.count = (uint8_t)n;
        memcpy(g_cmd.u.bmp_pixels.pixels, px + x, (size_t)n);
        send();
    }
}

static void sprite(int id, int x, int y, int pattern, uint8_t attr) {
    g_cmd.type = HALTZ_MSG_GPU_SPRITE_SET;
    g_cmd.u.sprite_set = (haltz_gpu_sprite_set_t){(uint8_t)id, (int16_t)x, (int16_t)y,
                                                  (uint8_t)pattern, attr};
    send();
}

static uint32_t g_rng;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int rr(int lo, int hi) {
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ---- patterns: 16 rows of 16 hex digits, '.' = 0 (transparent) ---------------------------- */

static void load_pattern(int index, const char *const rows[16]) {
    g_cmd.type = HALTZ_MSG_GPU_PATTERN_LOAD;
    haltz_gpu_pattern_load_t *m = &g_cmd.u.pattern_load;
    m->index = (uint8_t)index;
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
    send();
}

/* Colours in bank 0 are the system colours: 1 white, 2 red, 3 green, 4 blue, 5 yellow,
 * 6 cyan, 8 dark grey, 9 light grey, 10 orange, 11 brown, 12 pink, 13 light green,
 * 15 dark blue. The ball uses only 2, so other banks recolour it. */
static const char *const k_ball[16] = {
    "................", ".....222222.....", "...2222222222...", "..222222222222..",
    "..222222222222..", ".22222222222222.", ".22222222222222.", ".22222222222222.",
    ".22222222222222.", ".22222222222222.", ".22222222222222.", "..222222222222..",
    "..222222222222..", "...2222222222...", ".....222222.....", "................",
};
static const char *const k_run_a[16] = {
    "......BBBB......", ".....BBBBBB.....", ".....CCCCCC.....", ".....C8CC8C.....",
    ".....CCCCCC.....", "......CCCC......", "....44444444....", "...4444444444...",
    "..CC44444444CC..", "..C.44444444.C..", "....FFFFFFFF....", "....FFF..FFF....",
    "...FFF....FFF...", "..FFF......FFF..", ".888........888.", ".888........888.",
};
static const char *const k_run_b[16] = {
    "......BBBB......", ".....BBBBBB.....", ".....CCCCCC.....", ".....C8CC8C.....",
    ".....CCCCCC.....", "......CCCC......", "....44444444....", "....44444444....",
    "...C44444444C...", "...C44444444C...", "....FFFFFFFF....", ".....FFFFFF.....",
    ".....FFF.FF.....", ".....FFF.FF.....", ".....888.888....", ".....888.888....",
};
static const char *const k_ufo[16] = {
    "................", "................", "................", "......6666......",
    ".....666666.....", "....66666666....", "..999999999999..", ".99999999999999.",
    "9959959959959959", ".99999999999999.", "..999999999999..", "....88888888....",
    "................", "................", "................", "................",
};
static const char *const k_arrow[16] = {
    "8...............", "88..............", "818.............", "8118............",
    "81118...........", "811118..........", "8111118.........", "81111118........",
    "811111118.......", "8111111118......", "81111188888.....", "8118118.........",
    "818.8118........", "88..8118........", "8....8118.......", ".....888........",
};
static const char *const k_grass[16] = {
    "3D3D33D3D3D33D3D", "D3D3D3DD3D3D3D3D", "3333333333333333", "3B33B333B3333B33",
    "BBBBBBBBBBBBBBBB", "BBBB8BBBBBBBB8BB", "BBBBBBBBBBBBBBBB", "BBBBBBBABBBBBBBB",
    "B8BBBBBBBBBB8BBB", "BBBBBBBBBBBBBBBB", "BBBBBB8BBBBBBBBB", "BBABBBBBBBBBBABB",
    "BBBBBBBBBB8BBBBB", "BBBBBBBBBBBBBBBB", "BB8BBBBBBBBBBBBB", "BBBBBBBBBBBB8BBB",
};
static const char *const k_dirt[16] = {
    "BBBBBBBBBBBBBBBB", "BB8BBBBBBBABBBBB", "BBBBBBBBBBBBBBBB", "BBBBBB8BBBBBB8BB",
    "BBBBBBBBBBBBBBBB", "BABBBBBBBB8BBBBB", "BBBBBBBBBBBBBBBB", "BBBB8BBBBBBBBBAB",
    "BBBBBBBBBBBBBBBB", "BBBBBBBBABBBBBBB", "BB8BBBBBBBBBBBBB", "BBBBBBBBBBBB8BBB",
    "BBBBBBBBBBBBBBBB", "BBBBBABBBBBBBBBB", "BBBBBBBBBB8BBBBB", "BBBBBBBBBBBBBBBB",
};
static const char *const k_bush[16] = {
    "................", "................", "................", "................",
    "................", "................", "......3333......", "....33DD3333....",
    "...33DD333333...", "..3333333D3333..", ".33D3333333D333.", ".33333333333333.",
    "3333D3333333D333", "3333333333333333", "3333333333333333", "3333333333333333",
};

/* 8 x 8 glyphs, bit 7 = left pixel. */
static const uint8_t k_glyphs[4][HALTZ_GPU_GLYPH_BYTES] = {
    {0x00, 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18}, /* heart */
    {0x18, 0x1C, 0x16, 0x12, 0x10, 0x70, 0xF0, 0x60}, /* note */
    {0x10, 0x10, 0xFE, 0x7C, 0x38, 0x6C, 0x44, 0x00}, /* star */
    {0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C}, /* smiley */
};

/* ---- the still parts --------------------------------------------------------------------- */

/* Sky colour of bitmap row y: 13 bands of 16 pixels, dark at the top. */
static uint8_t sky(int y) {
    static const uint8_t bands[13] = {
        CUBE(0, 0, 1), CUBE(0, 0, 1), CUBE(0, 0, 2), CUBE(0, 0, 2), CUBE(0, 1, 2),
        CUBE(0, 1, 2), CUBE(0, 1, 3), CUBE(1, 1, 3), CUBE(1, 1, 3), CUBE(1, 2, 3),
        CUBE(1, 2, 4), CUBE(2, 2, 4), CUBE(2, 3, 4)};
    int b = y / 16;
    return bands[b < 13 ? b : 12];
}

static void draw_sky(void) {
    for (int b = 0; b < 13; b++) {
        fill(0, b * 16, W, 16, sky(b * 16));
    }
    /* Aurora: a wavy band, one BMP_PIXELS row at a time over the sky colour. */
    static uint8_t row[W];
    for (int y = 24; y < 80; y++) {
        for (int x = 0; x < W; x++) {
            float c = 44.0f + 14.0f * sinf((float)x * 0.025f) + 6.0f * sinf((float)x * 0.071f + 1.0f);
            float d = fabsf((float)y - c);
            row[x] = d < 2.0f ? CUBE(2, 5, 4) : d < 5.0f ? CUBE(0, 5, 3) : d < 9.0f ? CUBE(0, 3, 2)
                                                                                    : sky(y);
        }
        pixels_row(y, row);
    }
    /* Stars, in the twinkling colours. */
    for (int i = 0; i < 110; i++) {
        int x = rr(0, W - 1), y = rr(0, 140);
        fill(x, y, (i % 7) ? 1 : 2, 1, (uint8_t)(STAR_FIRST + i % STAR_COLS));
    }
    /* Moon with two craters (spans of a circle). */
    struct { int x, y, r; uint8_t c; } discs[3] = {
        {64, 44, 16, CUBE(5, 5, 4)}, {58, 40, 4, CUBE(4, 4, 3)}, {70, 50, 3, CUBE(4, 4, 3)}};
    for (int k = 0; k < 3; k++) {
        for (int dy = -discs[k].r; dy <= discs[k].r; dy++) {
            int half = (int)sqrtf((float)(discs[k].r * discs[k].r - dy * dy));
            fill(discs[k].x - half, discs[k].y + dy, 2 * half + 1, 1, discs[k].c);
        }
    }
    /* Mountains, then hills in front, as vertical lines down to the ground. */
    for (int x = 0; x < W; x++) {
        int far = 128 + (int)(26.0f * sinf((float)x * 0.021f + 0.6f) +
                              12.0f * sinf((float)x * 0.057f + 2.0f));
        line(x, far, x, GROUND_Y - 1, CUBE(1, 1, 2));
        int near = 176 + (int)(10.0f * sinf((float)x * 0.033f + 4.0f) +
                               5.0f * sinf((float)x * 0.11f));
        line(x, near, x, GROUND_Y - 1, CUBE(0, 1, 0));
    }
    /* Frame-rate graph: black panel with a grey frame. */
    fill(GRAPH_X - 1, GRAPH_Y - 1, GRAPH_W + 2, GRAPH_H + 2, HALTZ_COLOR_DARK_GREY);
    fill(GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, HALTZ_COLOR_BLACK);
}

static void draw_ground(void) {
    g_cmd.type = HALTZ_MSG_GPU_TILE_FILL;
    g_cmd.u.tile_fill = (haltz_gpu_tile_fill_t){0, 13, HALTZ_GPU_MAP_W, 1,
                                                HALTZ_GPU_TILE(PAT_GRASS, 0, 0)};
    send();
    g_cmd.u.tile_fill = (haltz_gpu_tile_fill_t){0, 14, HALTZ_GPU_MAP_W, 1,
                                                HALTZ_GPU_TILE(PAT_DIRT, 0, 0)};
    send();
    /* Bushes on map row 12, some mirrored: one TILE_PUT for the whole row. */
    uint16_t bushes[HALTZ_GPU_MAP_W] = {0};
    static const int at[] = {2, 3, 9, 15, 16, 22, 28};
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        bushes[at[i]] = HALTZ_GPU_TILE(PAT_BUSH, 0, (i & 1) ? HALTZ_GPU_TILE_HFLIP : 0);
    }
    g_cmd.type = HALTZ_MSG_GPU_TILE_PUT;
    g_cmd.u.tile_put.x = 0;
    g_cmd.u.tile_put.y = 12;
    g_cmd.u.tile_put.count = HALTZ_GPU_MAP_W;
    memcpy(g_cmd.u.tile_put.entries, bushes, sizeof(bushes));
    send();
}

static void draw_text(void) {
    char title[HALTZ_GPU_TEXT_COLS + 1];
    snprintf(title, sizeof(title), " %c HALTZ demo %c driven by the S3 %c%c", GLYPH_STAR,
             GLYPH_NOTE, GLYPH_HEART, GLYPH_SMILE);
    g_cmd.type = HALTZ_MSG_GPU_TEXT_FILL;
    g_cmd.u.text_fill = (haltz_gpu_text_fill_t){0, 0, HALTZ_GPU_TEXT_COLS, 1, ' ',
                                                HALTZ_COLOR_WHITE, HALTZ_COLOR_DARK_BLUE};
    send();
    text(0, 0, HALTZ_COLOR_WHITE, HALTZ_COLOR_DARK_BLUE, title);
    g_cmd.type = HALTZ_MSG_GPU_TEXT_FILL; /* text() changed it */
    g_cmd.u.text_fill = (haltz_gpu_text_fill_t){0, 29, HALTZ_GPU_TEXT_COLS, 1, ' ',
                                                HALTZ_COLOR_LIGHT_GREY, HALTZ_COLOR_DARK_BLUE};
    send();
    text(27, 29, HALTZ_COLOR_LIGHT_GREY, HALTZ_COLOR_DARK_BLUE, "any key: back");
    text(29, 6, HALTZ_COLOR_LIGHT_GREY, 0, "fps --");
}

/* ---- moving parts ------------------------------------------------------------------------ */

typedef struct {
    int32_t x, y, vx, vy; /* 1/256 pixel */
    uint8_t bank;
} ball_t;

static ball_t g_balls[BALLS];

#define FLOOR_Y ((GROUND_Y - 16) * 256)
#define GRAVITY 40 /* 1/256 pixel per frame per frame */

static void init_balls(void) {
    static const uint8_t banks[5] = {0, 2, 3, 6, 10}; /* red, green, teal, purple, pink */
    for (int i = 0; i < BALLS; i++) {
        ball_t *b = &g_balls[i];
        b->x = rr(0, W - 16) * 256;
        b->y = rr(20, 150) * 256;
        b->vx = rr(-300, 300);
        b->vy = 0;
        b->bank = banks[i % 5];
    }
}

static void move_balls(void) {
    for (int i = 0; i < BALLS; i++) {
        ball_t *b = &g_balls[i];
        b->vy += GRAVITY;
        b->x += b->vx;
        b->y += b->vy;
        if (b->y > FLOOR_Y) { /* bounce without losing energy, so they never stop */
            b->y = 2 * FLOOR_Y - b->y;
            b->vy = -b->vy;
        }
        if (b->x < 0 || b->x > (W - 16) * 256) {
            b->vx = -b->vx;
            b->x = b->x < 0 ? -b->x : 2 * (W - 16) * 256 - b->x;
        }
        uint8_t flip = b->vx < 0 ? HALTZ_GPU_SPR_HFLIP : 0;
        sprite(i, b->x / 256, b->y / 256, PAT_BALL,
               (uint8_t)(HALTZ_GPU_SPR_VISIBLE | b->bank | flip));
    }
}

static int tri(uint32_t v, int max) { /* triangle wave 0..max..0 */
    v %= (uint32_t)(2 * max);
    return (int)(v <= (uint32_t)max ? v : 2u * (uint32_t)max - v);
}

static void twinkle(uint32_t frame) {
    g_cmd.type = HALTZ_MSG_GPU_PALETTE;
    g_cmd.u.palette.first = STAR_FIRST;
    g_cmd.u.palette.count = STAR_COLS;
    for (int i = 0; i < STAR_COLS; i++) {
        int level = 90 + tri(frame / 2 + (uint32_t)i * 11, 165); /* grey 90..255 */
        g_cmd.u.palette.colors[i] = HALTZ_GPU_RGB565(level, level, level < 200 ? level : 255);
    }
    send();
}

static void graph_sample(int fps) {
    /* Move the graph left (BMP_COPY handles the overlap), then draw the new bar. */
    g_cmd.type = HALTZ_MSG_GPU_BMP_COPY;
    g_cmd.u.bmp_copy = (haltz_gpu_bmp_copy_t){GRAPH_X + GRAPH_STEP, GRAPH_Y,
                                              GRAPH_W - GRAPH_STEP, GRAPH_H, GRAPH_X, GRAPH_Y};
    send();
    int x = GRAPH_X + GRAPH_W - GRAPH_STEP;
    fill(x, GRAPH_Y, GRAPH_STEP, GRAPH_H, HALTZ_COLOR_BLACK);
    int v = fps > GRAPH_MAX_FPS ? GRAPH_MAX_FPS : fps;
    int h = v * GRAPH_H / GRAPH_MAX_FPS;
    uint8_t c = fps >= 25 ? HALTZ_COLOR_LIGHT_GREEN : fps >= 15 ? HALTZ_COLOR_YELLOW
                                                                : HALTZ_COLOR_RED;
    fill(x, GRAPH_Y + GRAPH_H - h, GRAPH_STEP - 1, h, c);
    char s[12];
    snprintf(s, sizeof(s), "fps %-3d", fps);
    text(29, 6, HALTZ_COLOR_LIGHT_GREY, 0, s);
}

/* ---- public ------------------------------------------------------------------------------ */

void demo_scene_init(demo_emit_fn emit, void *ctx) {
    g_emit = emit;
    g_ctx = ctx;
    g_rng = 0x48414C54u; /* "HALT": the same scene every time */

    g_cmd.type = HALTZ_MSG_GPU_INIT;
    send();
    g_cmd.type = HALTZ_MSG_GPU_LAYERS;
    g_cmd.u.layers.mask = HALTZ_GPU_LAYERS_ALL;
    send();

    g_cmd.type = HALTZ_MSG_GPU_FONT_LOAD;
    g_cmd.u.font_load.first = GLYPH_HEART;
    g_cmd.u.font_load.count = 4;
    memcpy(g_cmd.u.font_load.glyphs, k_glyphs, sizeof(k_glyphs));
    send();

    load_pattern(PAT_BALL, k_ball);
    load_pattern(PAT_RUN_A, k_run_a);
    load_pattern(PAT_RUN_B, k_run_b);
    load_pattern(PAT_UFO, k_ufo);
    load_pattern(PAT_ARROW, k_arrow);
    load_pattern(PAT_GRASS, k_grass);
    load_pattern(PAT_DIRT, k_dirt);
    load_pattern(PAT_BUSH, k_bush);

    draw_sky();
    draw_ground();
    draw_text();
    init_balls();
    demo_scene_step(0, -1, emit, ctx);
}

void demo_scene_step(uint32_t frame, int fps, demo_emit_fn emit, void *ctx) {
    g_emit = emit;
    g_ctx = ctx;

    g_cmd.type = HALTZ_MSG_GPU_TILE_SCROLL;
    g_cmd.u.tile_scroll = (haltz_gpu_tile_scroll_t){(uint16_t)(frame * 2), 0};
    send();

    move_balls();
    int bob = (frame / 4) % 2;
    sprite(SPR_RUNNER, 64, GROUND_Y - 16 - bob, (frame / 6) % 2 ? PAT_RUN_B : PAT_RUN_A,
           HALTZ_GPU_SPR_VISIBLE);
    int ux = (int)((frame * 3 / 2) % (W + 48)) - 32;
    int uy = 78 + (int)(8.0f * sinf((float)frame * 0.05f));
    sprite(SPR_UFO, ux, uy, PAT_UFO, HALTZ_GPU_SPR_VISIBLE);
    sprite(SPR_POINTER, 8 + tri(frame * 3, 280), tri(frame * 2, 44), PAT_ARROW,
           HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_ABOVE_TEXT);

    if (frame % 4 == 0) {
        twinkle(frame);
    }
    if (fps >= 0) {
        graph_sample(fps);
    }

    char s[32]; /* 24 cells, left of "any key: back" at column 27 */
    snprintf(s, sizeof(s), " frame %-6lu %d sprites", (unsigned long)(frame % 1000000u),
             DEMO_SCENE_SPRITES);
    text(0, 29, HALTZ_COLOR_LIGHT_GREY, HALTZ_COLOR_DARK_BLUE, s);
}
