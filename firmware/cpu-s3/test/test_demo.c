/*
 * PC test for demo_scene.c: every command it sends goes through the real wire path (pack,
 * frame encode, decode, haltz_gpu_cmd_unpack, i.e. what the GPU would NAK) into a gpu_state_t,
 * for 600 frames. With a folder argument it also renders frames with the GPU's renderer
 * (gpu_render.c) and writes them as PNG files, to look at the scene without the boards:
 *     ./build-cpu-test/cpu_demo_tests /tmp/demo
 */
#include <stdio.h>
#include <string.h>

#include "demo_scene.h"
#include "haltz/packet.h"

#include "../../gpu-pico/src/gpu_render.h"
#include "../../gpu-pico/src/gpu_state.h"

#define W HALTZ_GPU_SCREEN_W
#define H HALTZ_GPU_SCREEN_H

static int g_checks, g_fails;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        g_checks++;                                                                            \
        if (!(cond)) {                                                                         \
            g_fails++;                                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                      \
    } while (0)

/* ---- commands through the wire ----------------------------------------------------------- */

typedef struct {
    gpu_state_t *s;
    haltz_decoder_t dec;
    int refused; /* would be NAKed (or not even packed) */
    size_t commands, bytes;
} sink_t;

static void to_gpu(const haltz_gpu_cmd_t *cmd, void *ctx) {
    sink_t *k = ctx;
    uint8_t payload[HALTZ_MAX_PAYLOAD], frame[HALTZ_MAX_FRAME];
    size_t n = 0, fn = 0;
    if (haltz_gpu_cmd_pack(cmd, payload, sizeof(payload), &n) != HALTZ_OK ||
        haltz_frame_encode(cmd->type, payload, n, frame, sizeof(frame), &fn) != HALTZ_OK) {
        if (k->refused++ < 5) {
            printf("  refused: %s\n", haltz_msg_name(cmd->type));
        }
        return;
    }
    k->commands++;
    k->bytes += fn;
    for (size_t i = 0; i < fn; i++) {
        for (haltz_dec_result_t r = haltz_decoder_feed(&k->dec, frame[i]);
             r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&k->dec)) {
            static haltz_gpu_cmd_t got;
            const haltz_frame_t *f = haltz_decoder_frame(&k->dec);
            if (r != HALTZ_DEC_FRAME ||
                haltz_gpu_cmd_unpack(&got, f->type, f->payload, f->len) != HALTZ_OK) {
                k->refused++;
                continue;
            }
            gpu_state_apply(k->s, &got);
        }
    }
}

/* ---- PNG (stored deflate blocks, no zlib), as in firmware/gpu-pico/test ------------------- */

static uint16_t g_fb[H][W];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (table[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) {
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            }
            table[i] = v;
        }
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
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
    uint8_t b[8];
    be32(b, len);
    memcpy(b + 4, type, 4);
    fwrite(b, 1, 8, f);
    fwrite(data, 1, len, f);
    uint32_t c = crc32_update(crc32_update(0, (const uint8_t *)type, 4), data, len);
    be32(b, c);
    fwrite(b, 1, 4, f);
}

static void write_png(const char *dir, const char *name) {
    enum { SCALE = 2, PW = W * SCALE, PH = H * SCALE, ROW = 1 + PW * 3 };
    static uint8_t raw[ROW * PH];
    static uint8_t z[sizeof(raw) + sizeof(raw) / 65535 * 5 + 64];
    for (int y = 0; y < PH; y++) {
        uint8_t *r = &raw[(size_t)y * ROW];
        r[0] = 0;
        for (int x = 0; x < PW; x++) {
            uint16_t c = g_fb[y / SCALE][x / SCALE];
            r[1 + x * 3] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            r[2 + x * 3] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            r[3 + x * 3] = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < sizeof(raw);) {
        size_t n = sizeof(raw) - off > 65535 ? 65535 : sizeof(raw) - off;
        z[zn++] = (off + n == sizeof(raw)) ? 1 : 0;
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

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  cannot write %s\n", path);
        return;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0};
    be32(ihdr, PW);
    be32(ihdr + 4, PH);
    ihdr[8] = 8; /* 8 bits */
    ihdr[9] = 2; /* RGB */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    printf("  wrote %s\n", path);
}

/* ---- test -------------------------------------------------------------------------------- */

static gpu_state_t g_state;

int main(int argc, char **argv) {
    const char *png_dir = argc > 1 ? argv[1] : NULL;
    sink_t k = {.s = &g_state};
    haltz_decoder_init(&k.dec);
    gpu_state_init(&g_state);

    demo_scene_init(to_gpu, &k);
    printf("  init: %u commands, %u bytes on the wire\n", (unsigned)k.commands,
           (unsigned)k.bytes);
    CHECK(k.refused == 0);
    CHECK(g_state.layers == HALTZ_GPU_LAYERS_ALL);

    /* The status bar (row 29) is all dark-blue background: nothing else drew there. */
    int bar_ok = 1;
    for (int c = 0; c < HALTZ_GPU_TEXT_COLS; c++) {
        bar_ok &= g_state.text_bg[29][c] == HALTZ_COLOR_DARK_BLUE &&
                  g_state.text_bg[0][c] == HALTZ_COLOR_DARK_BLUE;
    }
    CHECK(bar_ok);

    int visible = 0;
    for (int i = 0; i < HALTZ_GPU_SPRITES; i++) {
        visible += (g_state.sprites[i].attr & HALTZ_GPU_SPR_VISIBLE) != 0;
    }
    CHECK(visible == DEMO_SCENE_SPRITES);

    size_t max_frame = 0;
    static const uint32_t shots[] = {0, 90, 333};
    for (uint32_t frame = 1; frame <= 600; frame++) {
        size_t before = k.bytes;
        demo_scene_step(frame, frame % 27 == 0 ? (int)(18 + frame % 13) : -1, to_gpu, &k);
        size_t used = k.bytes - before;
        max_frame = used > max_frame ? used : max_frame;
        for (size_t s = 0; png_dir != NULL && s < sizeof(shots) / sizeof(shots[0]); s++) {
            if (frame == shots[s] || (shots[s] == 0 && frame == 1)) {
                gpu_render_rect(&g_state, 0, 0, W, H, true, &g_fb[0][0]);
                char name[32];
                snprintf(name, sizeof(name), "demo_%03u.png", (unsigned)frame);
                write_png(png_dir, name);
            }
        }
    }
    printf("  600 frames: at most %u bytes per frame, %u refused\n", (unsigned)max_frame,
           (unsigned)k.refused);
    CHECK(k.refused == 0);
    CHECK(max_frame < HALTZ_GPU_MAX_XFER); /* a frame's changes fit one transaction */
    /* The pointer stays above the text, the balls stay above the ground. */
    CHECK(g_state.sprites[HALTZ_GPU_SPRITE_CURSOR].attr & HALTZ_GPU_SPR_ABOVE_TEXT);
    for (int i = 0; i < 40; i++) {
        CHECK(g_state.sprites[i].y <= 208 - 16 && g_state.sprites[i].x >= 0 &&
              g_state.sprites[i].x <= W - 16);
    }

    printf("%s demo scene: every command valid\n\n%d checks, %d failed\n",
           g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
