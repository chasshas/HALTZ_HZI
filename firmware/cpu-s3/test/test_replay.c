/*
 * PC test for gpu_replay.c: picture -> commands -> the same picture.
 *
 * For each of many random pictures (built from random valid commands of every kind, applied
 * with the GPU's own gpu_state code), the replay's commands are sent through the real wire
 * path (pack, frame encode, decode, haltz_gpu_cmd_unpack) into a fresh GPU_INIT state, which
 * must then equal the original in every field. Also checks a few pictures with known sizes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_replay.h"
#include "haltz/packet.h"

static int g_checks, g_fails;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        g_checks++;                                                                            \
        if (!(cond)) {                                                                         \
            g_fails++;                                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                      \
    } while (0)

/* ---- random valid commands --------------------------------------------------------------- */

static uint32_t g_rng = 12345;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int rr(int lo, int hi) {
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* Colours: often the defaults (white on 0), so the replay's "skip default cells" path runs. */
static uint8_t text_fg(void) {
    return (rnd() & 1) ? HALTZ_COLOR_WHITE : (uint8_t)rnd();
}

static uint8_t text_bg(void) {
    return (rnd() & 1) ? 0 : (uint8_t)rnd();
}

static uint8_t text_ch(void) {
    return (rnd() & 3) == 0 ? ' ' : (uint8_t)rnd();
}

static void random_cmd(haltz_gpu_cmd_t *c) {
    memset(c, 0, sizeof(*c));
    switch (rnd() % 20) {
    case 0:
        c->type = HALTZ_MSG_GPU_PALETTE;
        c->u.palette.count = (uint8_t)rr(1, HALTZ_GPU_PALETTE_MAX);
        c->u.palette.first = (uint8_t)rr(0, HALTZ_GPU_PALETTE_SIZE - c->u.palette.count);
        for (int i = 0; i < c->u.palette.count; i++) {
            c->u.palette.colors[i] = (rnd() & 1) ? (uint16_t)rnd()
                                                 : haltz_gpu_default_palette[c->u.palette.first + i];
        }
        break;
    case 1:
        c->type = HALTZ_MSG_GPU_FONT_LOAD;
        c->u.font_load.count = (uint8_t)rr(1, HALTZ_GPU_FONT_LOAD_MAX);
        c->u.font_load.first = (uint8_t)rr(0, HALTZ_GPU_FONT_GLYPHS - c->u.font_load.count);
        for (int i = 0; i < c->u.font_load.count; i++) {
            for (int b = 0; b < HALTZ_GPU_GLYPH_BYTES; b++) {
                c->u.font_load.glyphs[i][b] = (uint8_t)rnd();
            }
        }
        break;
    case 2:
        c->type = HALTZ_MSG_GPU_LAYERS;
        c->u.layers.mask = (uint8_t)(rnd() & HALTZ_GPU_LAYERS_ALL);
        break;
    case 3:
    case 4:
        c->type = HALTZ_MSG_GPU_TEXT_PUT;
        c->u.text_put.col = (uint8_t)rr(0, HALTZ_GPU_TEXT_COLS - 1);
        c->u.text_put.row = (uint8_t)rr(0, HALTZ_GPU_TEXT_ROWS - 1);
        c->u.text_put.fg = text_fg();
        c->u.text_put.bg = text_bg();
        c->u.text_put.count = (uint8_t)rr(1, HALTZ_GPU_TEXT_PUT_MAX);
        for (int i = 0; i < c->u.text_put.count; i++) {
            c->u.text_put.chars[i] = text_ch();
        }
        break;
    case 5:
        c->type = HALTZ_MSG_GPU_TEXT_FILL;
        c->u.text_fill = (haltz_gpu_text_fill_t){(uint8_t)rr(0, 45), (uint8_t)rr(0, 35),
                                                 (uint8_t)rr(0, 45), (uint8_t)rr(0, 35),
                                                 text_ch(), text_fg(), text_bg()};
        break;
    case 6:
        c->type = HALTZ_MSG_GPU_TEXT_SCROLL;
        c->u.text_scroll = (haltz_gpu_text_scroll_t){(uint8_t)rr(0, 31), (uint8_t)rr(0, 31),
                                                     (int8_t)rr(-35, 35), text_ch(), text_fg(),
                                                     text_bg()};
        break;
    case 7:
        c->type = HALTZ_MSG_GPU_TEXT_CURSOR;
        c->u.text_cursor = (haltz_gpu_text_cursor_t){
            (uint8_t)rr(0, 45), (uint8_t)rr(0, 35),
            (uint8_t)(rr(0, 2) | ((rnd() & 1) ? HALTZ_GPU_CURSOR_BLINK : 0))};
        if (rr(0, 3) == 0) {
            c->u.text_cursor = (haltz_gpu_text_cursor_t){0, 0, HALTZ_GPU_CURSOR_OFF};
        }
        break;
    case 8:
        c->type = HALTZ_MSG_GPU_PATTERN_LOAD;
        c->u.pattern_load.index = (uint8_t)rnd();
        for (int i = 0; i < HALTZ_GPU_PATTERN_BYTES; i++) {
            c->u.pattern_load.data[i] = (rr(0, 9) == 0) ? 0 : (uint8_t)rnd();
        }
        if (rr(0, 4) == 0) { /* an empty pattern: back to the default */
            memset(c->u.pattern_load.data, 0, HALTZ_GPU_PATTERN_BYTES);
        }
        break;
    case 9:
        c->type = HALTZ_MSG_GPU_TILE_PUT;
        c->u.tile_put.x = (uint8_t)rr(0, HALTZ_GPU_MAP_W - 1);
        c->u.tile_put.y = (uint8_t)rr(0, HALTZ_GPU_MAP_H - 1);
        c->u.tile_put.count = (uint8_t)rr(1, HALTZ_GPU_TILE_PUT_MAX);
        for (int i = 0; i < c->u.tile_put.count; i++) {
            c->u.tile_put.entries[i] = (rnd() & 1) ? 0 : (uint16_t)(rnd() & ~HALTZ_GPU_TILE_RESERVED);
        }
        break;
    case 10:
        c->type = HALTZ_MSG_GPU_TILE_FILL;
        c->u.tile_fill = (haltz_gpu_tile_fill_t){(uint8_t)rr(0, 40), (uint8_t)rr(0, 40),
                                                 (uint8_t)rr(0, 40), (uint8_t)rr(0, 40),
                                                 (uint16_t)(rnd() & ~HALTZ_GPU_TILE_RESERVED)};
        break;
    case 11:
        c->type = HALTZ_MSG_GPU_TILE_SCROLL;
        c->u.tile_scroll = (haltz_gpu_tile_scroll_t){(uint16_t)rnd(), (uint16_t)rnd()};
        break;
    case 12:
    case 13:
        c->type = HALTZ_MSG_GPU_SPRITE_SET;
        c->u.sprite_set = (haltz_gpu_sprite_set_t){(uint8_t)rr(0, HALTZ_GPU_SPRITES - 1),
                                                   (int16_t)rnd(), (int16_t)rnd(),
                                                   (uint8_t)rnd(), (uint8_t)rnd()};
        if (rr(0, 4) == 0) { /* back to the default (all 0) */
            c->u.sprite_set.x = c->u.sprite_set.y = 0;
            c->u.sprite_set.pattern = c->u.sprite_set.attr = 0;
        }
        break;
    case 14:
        c->type = HALTZ_MSG_GPU_BMP_PIXELS;
        c->u.bmp_pixels.x = (uint16_t)rr(0, 330);
        c->u.bmp_pixels.y = (uint16_t)rr(0, 245);
        c->u.bmp_pixels.count = (uint8_t)rr(1, HALTZ_GPU_BMP_PIXELS_MAX);
        for (int i = 0; i < c->u.bmp_pixels.count; i++) {
            c->u.bmp_pixels.pixels[i] = (rnd() & 3) ? (uint8_t)rnd() : 0;
        }
        break;
    case 15:
    case 16:
        c->type = HALTZ_MSG_GPU_BMP_FILL;
        if (rnd() & 1) { /* full-width bands: the replay merges these into one fill */
            c->u.bmp_fill = (haltz_gpu_bmp_fill_t){0, (uint16_t)rr(0, 239), 320,
                                                   (uint16_t)rr(1, 80), (uint8_t)rr(0, 3)};
        } else {
            c->u.bmp_fill = (haltz_gpu_bmp_fill_t){(uint16_t)rr(0, 330), (uint16_t)rr(0, 245),
                                                   (uint16_t)rr(0, 200), (uint16_t)rr(0, 200),
                                                   (uint8_t)rnd()};
        }
        break;
    case 17:
        c->type = HALTZ_MSG_GPU_BMP_LINE;
        c->u.bmp_line = (haltz_gpu_bmp_line_t){(int16_t)rr(-100, 420), (int16_t)rr(-100, 340),
                                               (int16_t)rr(-100, 420), (int16_t)rr(-100, 340),
                                               (uint8_t)rnd()};
        break;
    case 18:
        c->type = HALTZ_MSG_GPU_BMP_COPY;
        c->u.bmp_copy = (haltz_gpu_bmp_copy_t){(uint16_t)rr(0, 330), (uint16_t)rr(0, 245),
                                               (uint16_t)rr(0, 200), (uint16_t)rr(0, 200),
                                               (int16_t)rr(-100, 350), (int16_t)rr(-100, 270)};
        break;
    default:
        c->type = HALTZ_MSG_GPU_INIT; /* now and then everything back to the defaults */
        if (rr(0, 3) != 0) {
            c->type = HALTZ_MSG_GPU_TEXT_PUT;
            c->u.text_put = (haltz_gpu_text_put_t){.col = 0, .row = 0, .fg = 1, .count = 1};
            c->u.text_put.chars[0] = 'A';
        }
        break;
    }
}

/* ---- replay through the wire ------------------------------------------------------------- */

typedef struct {
    gpu_state_t *dst;
    size_t commands;
    size_t bytes; /* on the wire, frame overhead included */
    int bad;      /* commands that failed to pack / unpack */
    haltz_decoder_t dec;
} sink_t;

static void sink(const haltz_gpu_cmd_t *cmd, void *ctx) {
    sink_t *s = ctx;
    uint8_t payload[HALTZ_MAX_PAYLOAD];
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = 0, fn = 0;
    if (haltz_gpu_cmd_pack(cmd, payload, sizeof(payload), &n) != HALTZ_OK ||
        haltz_frame_encode(cmd->type, payload, n, frame, sizeof(frame), &fn) != HALTZ_OK) {
        s->bad++;
        return;
    }
    s->commands++;
    s->bytes += fn;
    int frames = 0;
    for (size_t i = 0; i < fn; i++) {
        for (haltz_dec_result_t r = haltz_decoder_feed(&s->dec, frame[i]);
             r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&s->dec)) {
            if (r != HALTZ_DEC_FRAME) {
                s->bad++;
                continue;
            }
            static haltz_gpu_cmd_t got;
            const haltz_frame_t *f = haltz_decoder_frame(&s->dec);
            if (haltz_gpu_cmd_unpack(&got, f->type, f->payload, f->len) != HALTZ_OK) {
                s->bad++;
                continue;
            }
            gpu_state_apply(s->dst, &got);
            frames++;
        }
    }
    if (frames != 1) {
        s->bad++;
    }
}

static int states_equal(const gpu_state_t *a, const gpu_state_t *b, int verbose) {
#define SAME(field)                                                                            \
    do {                                                                                       \
        if (memcmp(&a->field, &b->field, sizeof(a->field)) != 0) {                             \
            if (verbose) {                                                                     \
                printf("  differs: %s\n", #field);                                             \
            }                                                                                  \
            ok = 0;                                                                            \
        }                                                                                      \
    } while (0)
    int ok = 1;
    SAME(palette);
    SAME(font);
    SAME(layers);
    SAME(text_ch);
    SAME(text_fg);
    SAME(text_bg);
    SAME(cursor_col);
    SAME(cursor_row);
    SAME(cursor_style);
    SAME(patterns);
    SAME(pattern_rows);
    SAME(map);
    SAME(scroll_x);
    SAME(scroll_y);
    SAME(sprites);
    SAME(bitmap);
    return ok;
#undef SAME
}

static gpu_state_t g_a, g_b;
static haltz_gpu_cmd_t g_cmd, g_scratch;

/* Replay g_a into g_b; returns the sink for size checks. */
static sink_t replay_into_b(void) {
    sink_t s = {.dst = &g_b};
    haltz_decoder_init(&s.dec);
    gpu_state_init(&g_b);
    gpu_replay(&g_a, &g_scratch, sink, &s);
    return s;
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_default_picture(void) {
    gpu_state_init(&g_a);
    sink_t s = replay_into_b();
    CHECK(s.commands == 0);
    CHECK(states_equal(&g_a, &g_b, 1));
}

static void test_known_sizes(void) {
    /* One line of text: one TEXT_PUT with the line's characters only. */
    gpu_state_init(&g_a);
    g_cmd = (haltz_gpu_cmd_t){.type = HALTZ_MSG_GPU_TEXT_PUT};
    g_cmd.u.text_put = (haltz_gpu_text_put_t){.col = 3, .row = 5, .fg = 1, .bg = 0, .count = 5};
    memcpy(g_cmd.u.text_put.chars, "HALTZ", 5);
    gpu_state_apply(&g_a, &g_cmd);
    sink_t s = replay_into_b();
    CHECK(s.commands == 1 && s.bytes == HALTZ_FRAME_OVERHEAD + 4 + 5);
    CHECK(states_equal(&g_a, &g_b, 1));

    /* A full-screen bitmap of one colour: one BMP_FILL. */
    gpu_state_init(&g_a);
    g_cmd = (haltz_gpu_cmd_t){.type = HALTZ_MSG_GPU_BMP_FILL};
    g_cmd.u.bmp_fill = (haltz_gpu_bmp_fill_t){0, 0, 320, 240, 7};
    gpu_state_apply(&g_a, &g_cmd);
    s = replay_into_b();
    CHECK(s.commands == 1);
    CHECK(states_equal(&g_a, &g_b, 1));

    /* A full screen of text in one colour pair: one TEXT_PUT per row. */
    gpu_state_init(&g_a);
    g_cmd = (haltz_gpu_cmd_t){.type = HALTZ_MSG_GPU_TEXT_FILL};
    g_cmd.u.text_fill = (haltz_gpu_text_fill_t){0, 0, 40, 30, 'x', 3, 4};
    gpu_state_apply(&g_a, &g_cmd);
    s = replay_into_b();
    CHECK(s.commands == HALTZ_GPU_TEXT_ROWS);
    CHECK(states_equal(&g_a, &g_b, 1));

    /* Worst case, everything differs: still fits the protocol's "about 150 KB" shadow. */
    gpu_state_init(&g_a);
    for (int i = 0; i < 5000; i++) {
        random_cmd(&g_cmd);
        if (g_cmd.type != HALTZ_MSG_GPU_INIT) {
            gpu_state_apply(&g_a, &g_cmd);
        }
    }
    for (int y = 0; y < HALTZ_GPU_SCREEN_H; y++) {
        for (int x = 0; x < HALTZ_GPU_SCREEN_W; x++) {
            g_a.bitmap[y][x] = (uint8_t)(x * 7 + y * 13 + 1);
        }
    }
    s = replay_into_b();
    CHECK(s.bad == 0);
    CHECK(states_equal(&g_a, &g_b, 1));
    printf("  busy picture: %u commands, %u bytes on the wire\n", (unsigned)s.commands,
           (unsigned)s.bytes);
}

static void test_random_pictures(void) {
    const int pictures = 1000;
    size_t total_bytes = 0, max_bytes = 0;
    int failed = 0;
    for (int p = 0; p < pictures; p++) {
        gpu_state_init(&g_a);
        int n = rr(1, (p % 10 == 0) ? 2000 : 80);
        for (int i = 0; i < n; i++) {
            random_cmd(&g_cmd);
            uint8_t buf[HALTZ_MAX_PAYLOAD];
            size_t len;
            CHECK(haltz_gpu_cmd_pack(&g_cmd, buf, sizeof(buf), &len) == HALTZ_OK);
            gpu_state_apply(&g_a, &g_cmd);
        }
        sink_t s = replay_into_b();
        CHECK(s.bad == 0);
        int eq = states_equal(&g_a, &g_b, failed < 3);
        if (!eq) {
            if (failed < 3) {
                printf("  picture %d (%d commands) not reproduced\n", p, n);
            }
            failed++;
        }
        total_bytes += s.bytes;
        max_bytes = s.bytes > max_bytes ? s.bytes : max_bytes;
    }
    CHECK(failed == 0);
    printf("  %d pictures, %d not reproduced; replay %u bytes on average, %u at most\n",
           pictures, failed, (unsigned)(total_bytes / (size_t)pictures), (unsigned)max_bytes);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"replay of the default picture is empty", test_default_picture},
        {"replay sizes of known pictures", test_known_sizes},
        {"random pictures are reproduced exactly", test_random_pictures},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
