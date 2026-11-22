/* PC tests for the GPU picture state and renderer. Exit code = number of failed checks. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gpu_render.h"
#include "gpu_scenes.h"
#include "gpu_state.h"
#include "render_reference.h"

static int g_checks;
static int g_fails;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        g_checks++;                                                                            \
        if (!(cond)) {                                                                         \
            g_fails++;                                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                      \
    } while (0)

#define W HALTZ_GPU_SCREEN_W
#define H HALTZ_GPU_SCREEN_H

static gpu_state_t g_s;
static uint16_t g_fb[H][W];
static haltz_gpu_cmd_t g_cmd;

static void render_all(bool blink_on) {
    gpu_render_rect(&g_s, 0, 0, W, H, blink_on, &g_fb[0][0]);
}

static uint16_t pal(uint8_t i) {
    return haltz_gpu_default_palette[i];
}

static void put_text(int col, int row, uint8_t fg, uint8_t bg, const char *str) {
    g_cmd.type = HALTZ_MSG_GPU_TEXT_PUT;
    g_cmd.u.text_put.col = (uint8_t)col;
    g_cmd.u.text_put.row = (uint8_t)row;
    g_cmd.u.text_put.fg = fg;
    g_cmd.u.text_put.bg = bg;
    g_cmd.u.text_put.count = (uint8_t)strlen(str);
    memcpy(g_cmd.u.text_put.chars, str, strlen(str));
    CHECK(gpu_state_apply(&g_s, &g_cmd));
}

static void solid_pattern(uint8_t index, uint8_t value) {
    g_cmd.type = HALTZ_MSG_GPU_PATTERN_LOAD;
    g_cmd.u.pattern_load.index = index;
    memset(g_cmd.u.pattern_load.data, (value << 4) | value, HALTZ_GPU_PATTERN_BYTES);
    gpu_state_apply(&g_s, &g_cmd);
}

static void sprite(uint8_t id, int x, int y, uint8_t pattern, uint8_t attr) {
    g_cmd.type = HALTZ_MSG_GPU_SPRITE_SET;
    g_cmd.u.sprite_set = (haltz_gpu_sprite_set_t){id, (int16_t)x, (int16_t)y, pattern, attr};
    gpu_state_apply(&g_s, &g_cmd);
}

static void layers(uint8_t mask) {
    g_cmd.type = HALTZ_MSG_GPU_LAYERS;
    g_cmd.u.layers.mask = mask;
    gpu_state_apply(&g_s, &g_cmd);
}

/* ---- PNG writer (stored deflate blocks, no zlib needed) ---------------------------------- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (crc_table[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) {
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            }
            crc_table[i] = v;
        }
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    uint32_t c = crc32_update(0, (const uint8_t *)type, 4);
    c = crc32_update(c, data, len);
    uint8_t t[4];
    be32(t, c);
    fwrite(t, 1, 4, f);
}

/* Write g_fb as an RGB PNG, scaled up by 'scale' so single pixels are easy to see. */
static void write_png(const char *dir, const char *name, int scale) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  cannot write %s\n", path);
        return;
    }
    const int pw = W * scale, ph = H * scale;
    const size_t row_bytes = 1 + (size_t)pw * 3;
    static uint8_t raw[(1 + W * 3 * 3) * H * 3];
    for (int y = 0; y < ph; y++) {
        uint8_t *r = &raw[(size_t)y * row_bytes];
        r[0] = 0; /* filter: none */
        for (int x = 0; x < pw; x++) {
            uint16_t c = g_fb[y / scale][x / scale];
            r[1 + x * 3] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            r[2 + x * 3] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            r[3 + x * 3] = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    size_t raw_len = row_bytes * (size_t)ph;
    /* zlib stream: header, stored blocks of <= 65535 bytes, adler32 */
    static uint8_t z[sizeof(raw) + sizeof(raw) / 65535 * 5 + 64];
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len;) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        z[zn++] = (off + n == raw_len) ? 1 : 0;
        z[zn++] = (uint8_t)n;
        z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)~n;
        z[zn++] = (uint8_t)(~n >> 8);
        memcpy(&z[zn], &raw[off], n);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[off + i]) % 65521;
            b = (b + a) % 65521;
        }
        zn += n;
        off += n;
    }
    be32(&z[zn], (b << 16) | a);
    zn += 4;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)pw);
    be32(ihdr + 4, (uint32_t)ph);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* no interlace */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    printf("  wrote %s\n", path);
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_init(void) {
    gpu_state_init(&g_s);
    render_all(true);
    int all_backdrop = 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            all_backdrop &= g_fb[y][x] == pal(0);
        }
    }
    CHECK(all_backdrop);
    gpu_rect_t r[GPU_MAX_RECTS];
    CHECK(gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS) == 1);
    CHECK(r[0].x == 0 && r[0].y == 0 && r[0].w == W && r[0].h == H);
    CHECK(!gpu_state_is_dirty(&g_s));
    CHECK(gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS) == 0);
}

static void test_text_and_dirty(void) {
    gpu_state_init(&g_s);
    gpu_rect_t r[GPU_MAX_RECTS];
    gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);

    put_text(5, 2, HALTZ_COLOR_YELLOW, 0, "AB!");
    CHECK(gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS) == 1);
    CHECK(r[0].x == 40 && r[0].y == 16 && r[0].w == 24 && r[0].h == 8);

    render_all(true);
    /* 'A' row 0 = ..###... : columns 2-4 of the cell are lit */
    CHECK(g_fb[16][40 + 1] == pal(0));
    CHECK(g_fb[16][40 + 2] == pal(HALTZ_COLOR_YELLOW));
    CHECK(g_fb[16][40 + 4] == pal(HALTZ_COLOR_YELLOW));
    CHECK(g_fb[16][40 + 5] == pal(0));

    /* Wraps to the next row, stops at the end of the screen. */
    put_text(38, 0, 1, 0, "WXYZ");
    CHECK(g_s.text_ch[0][38] == 'W' && g_s.text_ch[0][39] == 'X');
    CHECK(g_s.text_ch[1][0] == 'Y' && g_s.text_ch[1][1] == 'Z');
    put_text(39, 29, 1, 0, "QR");
    CHECK(g_s.text_ch[29][39] == 'Q');

    /* Two separate places: two rectangles; the rows of one run merge. */
    gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    put_text(0, 10, 1, 0, "x");
    put_text(0, 11, 1, 0, "x");
    put_text(30, 20, 1, 0, "y");
    int n = gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    CHECK(n == 2);
    CHECK(r[0].x == 0 && r[0].y == 80 && r[0].w == 8 && r[0].h == 16);
    CHECK(r[1].x == 240 && r[1].y == 160);

    /* Too many separate rectangles (a checkerboard can't merge): one full-screen rectangle. */
    for (int row = 0; row < 10; row++) {
        for (int col = row % 2; col < 40; col += 2) {
            put_text(col, row, 1, 0, ".");
        }
    }
    n = gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    CHECK(n == 1 && r[0].w == W && r[0].h == H);
}

static void test_text_scroll_and_cursor(void) {
    gpu_state_init(&g_s);
    for (int row = 0; row < 30; row++) {
        char line[2] = {(char)('A' + row % 26), 0};
        put_text(0, row, 1, 0, line);
    }
    g_cmd.type = HALTZ_MSG_GPU_TEXT_SCROLL;
    g_cmd.u.text_scroll = (haltz_gpu_text_scroll_t){0, 29, 2, '-', 1, 0}; /* row 29 stays */
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.text_ch[0][0] == 'C' && g_s.text_ch[26][0] == 'C'); /* row 28 -> 26 */
    CHECK(g_s.text_ch[27][0] == '-' && g_s.text_ch[28][0] == '-');
    CHECK(g_s.text_ch[29][0] == 'D'); /* 'A' + 29 % 26 */

    g_cmd.u.text_scroll = (haltz_gpu_text_scroll_t){10, 5, -1, ' ', 1, 0};
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.text_ch[10][0] == ' ' && g_s.text_ch[11][0] == 'M'); /* old row 10 moved down */

    gpu_rect_t r[GPU_MAX_RECTS];
    gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    g_cmd.type = HALTZ_MSG_GPU_TEXT_CURSOR;
    g_cmd.u.text_cursor = (haltz_gpu_text_cursor_t){3, 4, HALTZ_GPU_CURSOR_BLOCK |
                                                              HALTZ_GPU_CURSOR_BLINK};
    gpu_state_apply(&g_s, &g_cmd);
    gpu_rect_t cr;
    CHECK(gpu_state_cursor_rect(&g_s, &cr) && cr.x == 24 && cr.y == 32);
    CHECK(gpu_state_cursor_blinks(&g_s));
    int n = gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS); /* old (0,0) + new cell */
    CHECK(n == 2);
    render_all(true);
    CHECK(g_fb[32][24] == pal(1)); /* space cell, inverse: fg everywhere */
    render_all(false);
    CHECK(g_fb[32][24] == pal(0)); /* blink off */
}

static void test_layers_order(void) {
    gpu_state_init(&g_s);
    layers(HALTZ_GPU_LAYERS_ALL);
    /* bitmap colour 20 everywhere in the top-left 32 x 32 */
    g_cmd.type = HALTZ_MSG_GPU_BMP_FILL;
    g_cmd.u.bmp_fill = (haltz_gpu_bmp_fill_t){0, 0, 32, 32, 20};
    gpu_state_apply(&g_s, &g_cmd);
    render_all(true);
    CHECK(g_fb[0][0] == pal(20));
    CHECK(g_fb[0][40] == pal(0));

    /* tile pattern 1 = solid value 3, bank 1 -> index 19, at map (1, 0) = screen x 16..31 */
    solid_pattern(1, 3);
    g_cmd.type = HALTZ_MSG_GPU_TILE_PUT;
    g_cmd.u.tile_put.x = 1;
    g_cmd.u.tile_put.y = 0;
    g_cmd.u.tile_put.count = 1;
    g_cmd.u.tile_put.entries[0] = HALTZ_GPU_TILE(1, 1, 0);
    gpu_state_apply(&g_s, &g_cmd);
    render_all(true);
    CHECK(g_fb[0][15] == pal(20)); /* tile 0 = pattern 0 = transparent: bitmap shows */
    CHECK(g_fb[0][16] == pal(19));

    /* sprite (pattern 2 solid 5, bank 0) at (20, 0): above tiles, below text */
    solid_pattern(2, 5);
    sprite(0, 20, 0, 2, HALTZ_GPU_SPR_VISIBLE);
    put_text(3, 0, HALTZ_COLOR_WHITE, HALTZ_COLOR_RED, " "); /* cell x 24..31: opaque bg */
    render_all(true);
    CHECK(g_fb[0][20] == pal(5));
    CHECK(g_fb[0][24] == pal(HALTZ_COLOR_RED)); /* text over sprite */
    sprite(0, 20, 0, 2, HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_ABOVE_TEXT);
    render_all(true);
    CHECK(g_fb[0][24] == pal(5)); /* sprite over text */

    /* higher sprite id is on top */
    solid_pattern(3, 6);
    sprite(1, 22, 0, 3, HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_ABOVE_TEXT);
    render_all(true);
    CHECK(g_fb[0][22] == pal(6) && g_fb[0][21] == pal(5));

    /* switching the sprite layer off hides both groups; layer change marks everything */
    gpu_rect_t r[GPU_MAX_RECTS];
    gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    layers(HALTZ_GPU_LAYER_BITMAP | HALTZ_GPU_LAYER_TILES | HALTZ_GPU_LAYER_TEXT);
    CHECK(gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS) == 1 && r[0].w == W);
    render_all(true);
    CHECK(g_fb[0][20] == pal(19) && g_fb[0][24] == pal(HALTZ_COLOR_RED));
}

static void test_tiles_scroll_flip(void) {
    gpu_state_init(&g_s);
    layers(HALTZ_GPU_LAYER_TILES);
    /* pattern 1: left half value 1, right half value 2 */
    g_cmd.type = HALTZ_MSG_GPU_PATTERN_LOAD;
    g_cmd.u.pattern_load.index = 1;
    for (int y = 0; y < 16; y++) {
        for (int b = 0; b < 8; b++) {
            g_cmd.u.pattern_load.data[y * 8 + b] = b < 4 ? 0x11 : 0x22;
        }
    }
    gpu_state_apply(&g_s, &g_cmd);
    g_cmd.type = HALTZ_MSG_GPU_TILE_PUT;
    g_cmd.u.tile_put = (haltz_gpu_tile_put_t){.x = 0, .y = 0, .count = 2};
    g_cmd.u.tile_put.entries[0] = HALTZ_GPU_TILE(1, 0, 0);
    g_cmd.u.tile_put.entries[1] = HALTZ_GPU_TILE(1, 0, HALTZ_GPU_TILE_HFLIP);
    gpu_state_apply(&g_s, &g_cmd);
    render_all(true);
    CHECK(g_fb[0][0] == pal(1) && g_fb[0][8] == pal(2));
    CHECK(g_fb[0][16] == pal(2) && g_fb[0][24] == pal(1)); /* flipped */

    /* scroll x by 8: screen x 0 shows map x 8 (right half of tile 0) */
    g_cmd.type = HALTZ_MSG_GPU_TILE_SCROLL;
    g_cmd.u.tile_scroll = (haltz_gpu_tile_scroll_t){8, 0};
    gpu_state_apply(&g_s, &g_cmd);
    render_all(true);
    CHECK(g_fb[0][0] == pal(2));
    /* scroll x by 504 (= -8): tile 0 starts at screen x 8, wrapping */
    g_cmd.u.tile_scroll = (haltz_gpu_tile_scroll_t){504, 0};
    gpu_state_apply(&g_s, &g_cmd);
    render_all(true);
    CHECK(g_fb[0][7] == pal(0) && g_fb[0][8] == pal(1));
    /* a tile change at that scroll marks the shifted position */
    gpu_rect_t r[GPU_MAX_RECTS];
    gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    g_cmd.type = HALTZ_MSG_GPU_TILE_FILL;
    g_cmd.u.tile_fill = (haltz_gpu_tile_fill_t){0, 0, 1, 1, 0};
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS) == 1 && r[0].x == 8 && r[0].w == 16);
}

static void test_sprites_clip_flip_dirty(void) {
    gpu_state_init(&g_s);
    /* pattern 4: only the top-left pixel is set (value 7) */
    g_cmd.type = HALTZ_MSG_GPU_PATTERN_LOAD;
    memset(g_cmd.u.pattern_load.data, 0, HALTZ_GPU_PATTERN_BYTES);
    g_cmd.u.pattern_load.index = 4;
    g_cmd.u.pattern_load.data[0] = 0x70;
    gpu_state_apply(&g_s, &g_cmd);
    sprite(5, 100, 50, 4, HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_HFLIP | HALTZ_GPU_SPR_VFLIP);
    render_all(true);
    CHECK(g_fb[65][115] == pal(7)); /* flipped to the bottom-right */
    CHECK(g_fb[50][100] == pal(0));

    /* partly off screen: no crash, visible part drawn */
    sprite(6, -15, -15, 4, HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_HFLIP | HALTZ_GPU_SPR_VFLIP);
    sprite(7, W - 1, H - 1, 4, HALTZ_GPU_SPR_VISIBLE);
    render_all(true);
    CHECK(g_fb[0][0] == pal(7));
    CHECK(g_fb[H - 1][W - 1] == pal(7));

    /* moving a sprite marks the old and the new place */
    gpu_rect_t r[GPU_MAX_RECTS];
    gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    sprite(5, 200, 50, 4, HALTZ_GPU_SPR_VISIBLE);
    int n = gpu_state_take_dirty(&g_s, r, GPU_MAX_RECTS);
    CHECK(n == 2 && r[0].x == 96 && r[1].x == 200);
}

static void test_bitmap_ops(void) {
    gpu_state_init(&g_s);
    layers(HALTZ_GPU_LAYER_BITMAP);
    g_cmd.type = HALTZ_MSG_GPU_BMP_LINE;
    g_cmd.u.bmp_line = (haltz_gpu_bmp_line_t){10, 10, 13, 12, 9};
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[10][10] == 9 && g_s.bitmap[12][13] == 9); /* both ends */
    g_cmd.u.bmp_line = (haltz_gpu_bmp_line_t){-100, 5, 1000, 5, 4}; /* clipped */
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[5][0] == 4 && g_s.bitmap[5][W - 1] == 4);

    g_cmd.type = HALTZ_MSG_GPU_BMP_PIXELS;
    g_cmd.u.bmp_pixels.x = W - 2;
    g_cmd.u.bmp_pixels.y = 0;
    g_cmd.u.bmp_pixels.count = 5;
    memcpy(g_cmd.u.bmp_pixels.pixels, "\x01\x02\x03\x04\x05", 5);
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[0][W - 2] == 1 && g_s.bitmap[0][W - 1] == 2 && g_s.bitmap[1][0] == 0);

    /* rows 0..9 hold their row number; copy down by 1 overlapping */
    for (int y = 0; y < 10; y++) {
        memset(g_s.bitmap[y], y + 1, W);
    }
    g_cmd.type = HALTZ_MSG_GPU_BMP_COPY;
    g_cmd.u.bmp_copy = (haltz_gpu_bmp_copy_t){0, 0, W, 9, 0, 1};
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[0][0] == 1 && g_s.bitmap[1][0] == 1 && g_s.bitmap[9][0] == 9);
    /* and up by 1 */
    g_cmd.u.bmp_copy = (haltz_gpu_bmp_copy_t){0, 1, W, 9, 0, 0};
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[0][0] == 1 && g_s.bitmap[1][0] == 2 && g_s.bitmap[8][0] == 9);
    /* left by 3 with the destination partly off screen */
    g_cmd.u.bmp_copy = (haltz_gpu_bmp_copy_t){0, 0, 10, 1, -3, 0};
    g_s.bitmap[0][3] = 77;
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[0][0] == 77);

    g_cmd.type = HALTZ_MSG_GPU_BMP_FILL;
    g_cmd.u.bmp_fill = (haltz_gpu_bmp_fill_t){300, 230, 100, 100, 3}; /* clipped */
    gpu_state_apply(&g_s, &g_cmd);
    CHECK(g_s.bitmap[H - 1][W - 1] == 3 && g_s.bitmap[229][300] != 3);
}

static void test_render_rect_matches_full(void) {
    /* Rendering a sub-rectangle gives the same pixels as the full screen. */
    gpu_scene_demo_init(&g_s);
    gpu_scene_demo_step(&g_s, 77);
    render_all(true);
    static uint16_t part[37 * 23];
    gpu_render_rect(&g_s, 101, 53, 37, 23, true, part);
    int same = 1;
    for (int y = 0; y < 23; y++) {
        for (int x = 0; x < 37; x++) {
            same &= part[y * 37 + x] == g_fb[53 + y][101 + x];
        }
    }
    CHECK(same);
}

/* ---- fast renderer == reference renderer, on random pictures ----------------------------- */

static uint32_t g_rng = 0x1234567u;

static uint32_t rnd(void) { /* xorshift32: same sequence every run */
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void random_picture(void) {
    gpu_state_init(&g_s);
    g_s.layers = (uint8_t)(rnd() & HALTZ_GPU_LAYERS_ALL);
    for (int i = 0; i < HALTZ_GPU_PALETTE_SIZE; i++) {
        g_s.palette[i] = (uint16_t)rnd();
    }
    /* Patterns 0..15 through the command (it keeps pattern_rows up to date): empty, full of
     * noise, or sparse. */
    for (int p = 0; p < 16; p++) {
        g_cmd.type = HALTZ_MSG_GPU_PATTERN_LOAD;
        g_cmd.u.pattern_load.index = (uint8_t)p;
        int kind = (int)(rnd() % 3);
        for (int b = 0; b < HALTZ_GPU_PATTERN_BYTES; b++) {
            uint8_t v = (uint8_t)rnd();
            g_cmd.u.pattern_load.data[b] = kind == 0 ? 0 : kind == 1 ? v : (rnd() % 8 ? 0 : v);
        }
        gpu_state_apply(&g_s, &g_cmd);
    }
    for (int y = 0; y < HALTZ_GPU_MAP_H; y++) {
        for (int x = 0; x < HALTZ_GPU_MAP_W; x++) {
            g_s.map[y][x] = (uint16_t)((rnd() & 0x3F0F) & ~HALTZ_GPU_TILE_RESERVED);
        }
    }
    g_s.scroll_x = (uint16_t)(rnd() & 511);
    g_s.scroll_y = (uint16_t)(rnd() & 511);
    for (int i = 0; i < HALTZ_GPU_SPRITES; i++) {
        g_s.sprites[i].x = (int16_t)((int)(rnd() % 360) - 20);
        g_s.sprites[i].y = (int16_t)((int)(rnd() % 280) - 20);
        g_s.sprites[i].pattern = (uint8_t)(rnd() % 16);
        g_s.sprites[i].attr = (uint8_t)rnd();
    }
    for (int r = 0; r < HALTZ_GPU_TEXT_ROWS; r++) {
        for (int c = 0; c < HALTZ_GPU_TEXT_COLS; c++) {
            g_s.text_ch[r][c] = (uint8_t)rnd();
            g_s.text_fg[r][c] = (uint8_t)(rnd() % 3 ? rnd() : 0);
            g_s.text_bg[r][c] = (uint8_t)(rnd() % 3 ? 0 : rnd());
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            g_s.bitmap[y][x] = (uint8_t)((x / 7 + y / 5) % 4 ? 0 : rnd());
        }
    }
    g_s.cursor_col = (uint8_t)(rnd() % 44);
    g_s.cursor_row = (uint8_t)(rnd() % 32);
    g_s.cursor_style = (uint8_t)((rnd() % 3) | (rnd() % 2 ? HALTZ_GPU_CURSOR_BLINK : 0));
}

static void test_fast_equals_reference(void) {
    static uint16_t ref[H][W];
    static uint16_t fast[W * H];
    int bad_pictures = 0;
    for (int pic = 0; pic < 300; pic++) {
        random_picture();
        bool blink_on = rnd() % 2;
        ref_render_rect(&g_s, 0, 0, W, H, blink_on, &ref[0][0]);
        gpu_render_rect(&g_s, 0, 0, W, H, blink_on, &g_fb[0][0]);
        int bad = memcmp(ref, g_fb, sizeof(ref)) != 0;
        /* and a few odd-sized pieces, as the strip renderer asks for */
        for (int k = 0; k < 8 && !bad; k++) {
            int x = (int)(rnd() % W), y = (int)(rnd() % H);
            int w = 1 + (int)(rnd() % (unsigned)(W - x)), h = 1 + (int)(rnd() % (unsigned)(H - y));
            gpu_render_rect(&g_s, x, y, w, h, blink_on, fast);
            for (int r = 0; r < h && !bad; r++) {
                bad = memcmp(&fast[r * w], &ref[y + r][x], (size_t)w * 2) != 0;
            }
        }
        if (bad && bad_pictures++ == 0) {
            printf("  first difference in random picture %d (layers 0x%X)\n", pic, g_s.layers);
        }
    }
    CHECK(bad_pictures == 0);
}

int main(int argc, char **argv) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"init: backdrop, full dirty rect", test_init},
        {"text, wrapping, dirty rectangles", test_text_and_dirty},
        {"text scroll and cursor", test_text_scroll_and_cursor},
        {"layer order and transparency", test_layers_order},
        {"tiles: scroll, wrap, flip", test_tiles_scroll_flip},
        {"sprites: clip, flip, dirty", test_sprites_clip_flip_dirty},
        {"bitmap: line, pixels, copy, fill", test_bitmap_ops},
        {"sub-rectangle render = full render", test_render_rect_matches_full},
        {"fast renderer = reference renderer (300 random pictures)", test_fast_equals_reference},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }

    if (argc > 1) {
        gpu_scene_boot(&g_s, HALTZ_PROTO_VERSION, "1a2b3c4d");
        render_all(true);
        write_png(argv[1], "boot.png", 2);
        gpu_scene_demo_init(&g_s);
        for (uint32_t f = 0; f <= 60; f++) {
            gpu_scene_demo_step(&g_s, f);
        }
        render_all(true);
        write_png(argv[1], "demo_f60.png", 2);
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
