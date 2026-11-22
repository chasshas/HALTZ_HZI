/* PC unit tests for haltz_proto. No framework: each CHECK prints on failure; exit code = fails. */
#include <stdio.h>
#include <string.h>

#include "haltz/board_ids.h"
#include "haltz/crc8.h"
#include "haltz/gpu.h"
#include "haltz/gpu_msgs.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"
#include "haltz/proto.h"

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

/* Collected decoder output for one test. */
typedef struct {
    int frames;
    int crc_errors;
    haltz_frame_t last[8];
} events_t;

static void on_result(haltz_decoder_t *d, haltz_dec_result_t r, events_t *ev) {
    for (; r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(d)) {
        if (r == HALTZ_DEC_FRAME) {
            if (ev->frames < 8) {
                ev->last[ev->frames] = *haltz_decoder_frame(d);
            }
            ev->frames++;
        } else if (r == HALTZ_DEC_CRC_ERROR) {
            ev->crc_errors++;
        }
    }
}

static void feed_all(haltz_decoder_t *d, const uint8_t *buf, size_t n, events_t *ev) {
    for (size_t i = 0; i < n; i++) {
        on_result(d, haltz_decoder_feed(d, buf[i]), ev);
    }
}

static size_t make_frame(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out) {
    size_t n = 0;
    haltz_err_t err = haltz_frame_encode(type, payload, len, out, HALTZ_MAX_FRAME, &n);
    CHECK(err == HALTZ_OK);
    return n;
}

static int same_frame(const haltz_frame_t *f, uint8_t type, const uint8_t *payload, size_t len) {
    return f->type == type && f->len == len && (len == 0 || memcmp(f->payload, payload, len) == 0);
}

/* ---------------------------------------------------------------------------------------- */

static void test_crc8_vector(void) {
    const uint8_t check[] = "123456789";
    CHECK(haltz_crc8(check, 9) == HALTZ_CRC8_CHECK);
    CHECK(haltz_crc8(check, 9) == 0xF4);
    CHECK(haltz_crc8(NULL, 0) == 0x00);
    /* Byte-wise update gives the same result as the buffer version. */
    uint8_t crc = HALTZ_CRC8_INIT;
    for (int i = 0; i < 9; i++) {
        crc = haltz_crc8_update(crc, check[i]);
    }
    CHECK(crc == 0xF4);
    /* Single byte 0x01 with poly 0x07 -> 0x07. */
    const uint8_t one = 0x01;
    CHECK(haltz_crc8(&one, 1) == 0x07);
}

static void test_msg_tables(void) {
    CHECK(HALTZ_MSG_COUNT == 24);
    CHECK(HALTZ_MSG_HELLO == 0x01 && HALTZ_MSG_ACK == 0x02 && HALTZ_MSG_NAK == 0x03);
    CHECK(HALTZ_MSG_HELLO_LEN == 7 && HALTZ_MSG_ACK_LEN == 1 && HALTZ_MSG_NAK_LEN == 2);
    CHECK(HALTZ_MSG_HELLO_MIN_LEN == 7 && HALTZ_MSG_HELLO_MAX_LEN == 7);
    CHECK(strcmp(haltz_msg_name(0x01), "HELLO") == 0);
    CHECK(strcmp(haltz_msg_name(0x03), "NAK") == 0);
    CHECK(strcmp(haltz_msg_name(0x7E), "?") == 0);
    CHECK(haltz_msg_min_len(0x02) == 1 && haltz_msg_max_len(0x02) == 1);
    CHECK(haltz_msg_min_len(0x00) == -1 && haltz_msg_max_len(0x00) == -1);
    CHECK(haltz_msg_check(HALTZ_MSG_HELLO, 7) == HALTZ_OK);
    CHECK(haltz_msg_check(HALTZ_MSG_HELLO, 6) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_msg_check(HALTZ_MSG_HELLO, 8) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_msg_check(0xEE, 0) == HALTZ_ERR_UNKNOWN_TYPE);
    CHECK(HALTZ_BOARD_CPU == 1 && HALTZ_BOARD_IO == 2 && HALTZ_BOARD_GPU == 3);
}

/* GPU link messages (docs/gpu-protocol-v1.md section 4). */
static void test_gpu_msg_table(void) {
    /* Every GPU_* message sits in the GPU range, everything else in the common range. */
    for (int i = 0; i < HALTZ_MSG_COUNT; i++) {
        int is_gpu = strncmp(haltz_msg_names[i], "GPU_", 4) == 0;
        if (is_gpu) {
            CHECK(haltz_msg_ids[i] >= 0x10 && haltz_msg_ids[i] <= 0x3F);
        } else {
            CHECK(haltz_msg_ids[i] >= 0x01 && haltz_msg_ids[i] <= 0x0F);
        }
    }

    /* Variable-length messages: MAX_LEN = header + the most elements that fit in 255. */
    CHECK(HALTZ_MSG_GPU_PALETTE_MAX_LEN == 1 + 2 * 127);
    CHECK(HALTZ_MSG_GPU_TEXT_PUT_MAX_LEN == 4 + 251);
    CHECK(HALTZ_MSG_GPU_FONT_LOAD_MIN_LEN == 1 + HALTZ_GPU_GLYPH_BYTES);
    CHECK(HALTZ_MSG_GPU_FONT_LOAD_MAX_LEN == 1 + 31 * HALTZ_GPU_GLYPH_BYTES);
    CHECK(HALTZ_MSG_GPU_TILE_PUT_MAX_LEN == 2 + 2 * 126);
    CHECK(HALTZ_MSG_GPU_BMP_PIXELS_MAX_LEN == 4 + 251);
    CHECK(HALTZ_MSG_GPU_PATTERN_LOAD_LEN == 1 + HALTZ_GPU_PATTERN_BYTES);

    /* Fixed sizes from the summary table. */
    CHECK(HALTZ_MSG_GPU_INIT_LEN == 0 && HALTZ_MSG_GPU_PRESENT_LEN == 2);
    CHECK(HALTZ_MSG_GPU_SPRITE_SET_LEN == 7 && HALTZ_MSG_GPU_BMP_COPY_LEN == 12);
    CHECK(HALTZ_MSG_GPU_STATUS_LEN == 18);

    /* Range checks. */
    CHECK(haltz_msg_check(HALTZ_MSG_GPU_TEXT_PUT, 4) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_msg_check(HALTZ_MSG_GPU_TEXT_PUT, 5) == HALTZ_OK);
    CHECK(haltz_msg_check(HALTZ_MSG_GPU_TEXT_PUT, 255) == HALTZ_OK);
    CHECK(haltz_msg_check(HALTZ_MSG_GPU_FONT_LOAD, 250) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_msg_check(HALTZ_MSG_GPU_INIT, 0) == HALTZ_OK);
    CHECK(haltz_msg_check(HALTZ_MSG_GPU_INIT, 1) == HALTZ_ERR_BAD_LEN);
    CHECK(strcmp(haltz_msg_name(0x3F), "GPU_STATUS") == 0);

    /* Constants. */
    CHECK(HALTZ_GPU_TEXT_COLS * HALTZ_GPU_GLYPH_SIZE == HALTZ_GPU_SCREEN_W);
    CHECK(HALTZ_GPU_TEXT_ROWS * HALTZ_GPU_GLYPH_SIZE == HALTZ_GPU_SCREEN_H);
    CHECK(HALTZ_GPU_MAP_PX == 512);
    CHECK(HALTZ_GPU_RGB565(255, 255, 255) == 0xFFFF && HALTZ_GPU_RGB565(255, 0, 0) == 0xF800);
    CHECK(HALTZ_GPU_RGB565(0, 255, 0) == 0x07E0 && HALTZ_GPU_RGB565(0, 0, 255) == 0x001F);
    CHECK(HALTZ_GPU_TILE(0xAB, 5, HALTZ_GPU_TILE_VFLIP) == 0x25AB);
    CHECK((HALTZ_GPU_TILE(0xFF, 0xF, 0xFFFF) & HALTZ_GPU_TILE_RESERVED) == 0);
}

static void test_hello_roundtrip(void) {
    haltz_hello_t in = {.board_id = HALTZ_BOARD_GPU, .proto_version = 0x1234};
    CHECK(haltz_git_hash_parse("1a2B3c4d", in.git_hash) == HALTZ_OK);
    CHECK(in.git_hash[0] == 0x1a && in.git_hash[3] == 0x4d);

    uint8_t buf[HALTZ_MSG_HELLO_LEN];
    CHECK(haltz_hello_pack(&in, buf, sizeof(buf)) == HALTZ_OK);
    /* Exact wire bytes: board, version little-endian, hash bytes in order. */
    const uint8_t expect[] = {0x03, 0x34, 0x12, 0x1a, 0x2b, 0x3c, 0x4d};
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);

    haltz_hello_t out;
    memset(&out, 0xEE, sizeof(out));
    CHECK(haltz_hello_unpack(&out, buf, sizeof(buf)) == HALTZ_OK);
    CHECK(out.board_id == in.board_id);
    CHECK(out.proto_version == in.proto_version);
    CHECK(memcmp(out.git_hash, in.git_hash, 4) == 0);

    char hex[9];
    haltz_git_hash_format(out.git_hash, hex);
    CHECK(strcmp(hex, "1a2b3c4d") == 0);

    CHECK(haltz_hello_pack(&in, buf, 6) == HALTZ_ERR_NO_SPACE);
    CHECK(haltz_hello_unpack(&out, buf, 6) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_hello_unpack(&out, buf, 8) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_hello_pack(NULL, buf, sizeof(buf)) == HALTZ_ERR_ARG);

    uint8_t h[4];
    CHECK(haltz_git_hash_parse("00000000", h) == HALTZ_OK && h[0] == 0 && h[3] == 0);
    CHECK(haltz_git_hash_parse("1234", h) == HALTZ_ERR_BAD_VALUE);
    CHECK(haltz_git_hash_parse("1234567g", h) == HALTZ_ERR_BAD_VALUE);
}

static void test_ack_nak_roundtrip(void) {
    uint8_t buf[2];
    haltz_ack_t ack = {.type = HALTZ_MSG_HELLO}, ack2 = {0};
    CHECK(haltz_ack_pack(&ack, buf, sizeof(buf)) == HALTZ_OK && buf[0] == 0x01);
    CHECK(haltz_ack_unpack(&ack2, buf, HALTZ_MSG_ACK_LEN) == HALTZ_OK && ack2.type == 0x01);

    haltz_nak_t nak = {.type = HALTZ_MSG_HELLO, .error = HALTZ_ERR_VERSION}, nak2 = {0};
    CHECK(haltz_nak_pack(&nak, buf, sizeof(buf)) == HALTZ_OK);
    CHECK(buf[0] == 0x01 && buf[1] == HALTZ_ERR_VERSION);
    CHECK(haltz_nak_unpack(&nak2, buf, 2) == HALTZ_OK);
    CHECK(nak2.type == nak.type && nak2.error == nak.error);
}

static void test_encode_then_decode(void) {
    haltz_hello_t hello = {.board_id = HALTZ_BOARD_CPU, .proto_version = HALTZ_PROTO_VERSION,
                           .git_hash = {0xde, 0xad, 0xbe, 0xef}};
    uint8_t payload[HALTZ_MSG_HELLO_LEN];
    CHECK(haltz_hello_pack(&hello, payload, sizeof(payload)) == HALTZ_OK);

    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = make_frame(HALTZ_MSG_HELLO, payload, sizeof(payload), frame);
    CHECK(n == 7 + HALTZ_FRAME_OVERHEAD);
    CHECK(frame[0] == HALTZ_SYNC && frame[1] == HALTZ_MSG_HELLO && frame[2] == 7);
    CHECK(frame[n - 1] == haltz_crc8(&frame[1], n - 2));

    haltz_decoder_t d;
    haltz_decoder_init(&d);
    events_t ev = {0};
    feed_all(&d, frame, n, &ev);
    CHECK(ev.frames == 1 && ev.crc_errors == 0);
    CHECK(same_frame(&ev.last[0], HALTZ_MSG_HELLO, payload, sizeof(payload)));

    haltz_hello_t back;
    CHECK(haltz_msg_check(ev.last[0].type, ev.last[0].len) == HALTZ_OK);
    CHECK(haltz_hello_unpack(&back, ev.last[0].payload, ev.last[0].len) == HALTZ_OK);
    CHECK(back.board_id == HALTZ_BOARD_CPU && back.proto_version == HALTZ_PROTO_VERSION);
    CHECK(back.git_hash[0] == 0xde && back.git_hash[3] == 0xef);

    /* Encoder argument checks. */
    CHECK(haltz_frame_encode(1, payload, 7, frame, 10, NULL) == HALTZ_ERR_NO_SPACE);
    CHECK(haltz_frame_encode(1, payload, 256, frame, sizeof(frame), NULL) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_frame_encode(1, NULL, 3, frame, sizeof(frame), NULL) == HALTZ_ERR_ARG);
    CHECK(haltz_frame_encode(1, payload, 0, NULL, 0, NULL) == HALTZ_ERR_ARG);
}

static void test_resync_after_garbage(void) {
    const uint8_t p[] = {0x42};
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = make_frame(HALTZ_MSG_ACK, p, 1, frame);

    /* Plain garbage without SYNC bytes. */
    haltz_decoder_t d;
    haltz_decoder_init(&d);
    events_t ev = {0};
    const uint8_t junk[] = {0x00, 0xFF, 0x13, 0x37, 0x5A};
    feed_all(&d, junk, sizeof(junk), &ev);
    feed_all(&d, frame, n, &ev);
    CHECK(ev.frames == 1 && same_frame(&ev.last[0], HALTZ_MSG_ACK, p, 1));
    CHECK(d.bytes_skipped == sizeof(junk));

    /* Garbage that contains SYNC (0xA5), directly followed by two real frames. The false
     * candidate reads TYPE=0x22, LEN=0xA5 (the real SYNC) and waits for 165 bytes that never
     * come, so the real frames are held back until the line goes idle and the transport calls
     * haltz_decoder_flush(). The decoder must then replay the bytes and find both frames. */
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    const uint8_t junk2[] = {0x11, HALTZ_SYNC, 0x22};
    feed_all(&d, junk2, sizeof(junk2), &ev);
    feed_all(&d, frame, n, &ev);
    feed_all(&d, frame, n, &ev);
    CHECK(ev.frames == 0); /* held back behind the false candidate */
    on_result(&d, haltz_decoder_flush(&d), &ev);
    CHECK(ev.frames == 2 && d.flushes == 1);
    CHECK(same_frame(&ev.last[0], HALTZ_MSG_ACK, p, 1));
    CHECK(same_frame(&ev.last[1], HALTZ_MSG_ACK, p, 1));
    on_result(&d, haltz_decoder_flush(&d), &ev); /* idle again: nothing in progress, no-op */
    CHECK(ev.frames == 2 && d.flushes == 1);

    /* Same garbage, but instead of an idle flush more line noise arrives (no SYNC in it).
     * The false candidate fails its CRC eventually and the replay still finds both frames. */
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    feed_all(&d, junk2, sizeof(junk2), &ev);
    feed_all(&d, frame, n, &ev);
    feed_all(&d, frame, n, &ev);
    uint8_t noise[200];
    memset(noise, 0x00, sizeof(noise));
    feed_all(&d, noise, sizeof(noise), &ev);
    CHECK(ev.frames == 2 && ev.crc_errors >= 1);

    /* A lone SYNC right before the frame: [A5][A5 02 01 42 crc]. The false candidate takes the
     * real SYNC as TYPE; after its CRC fails the real frame must be recovered. */
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    const uint8_t sync = HALTZ_SYNC;
    feed_all(&d, &sync, 1, &ev);
    feed_all(&d, frame, n, &ev);
    /* The false frame may still be waiting for bytes: flush it with idle-line garbage. */
    uint8_t idle[HALTZ_MAX_FRAME];
    memset(idle, 0x00, sizeof(idle));
    feed_all(&d, idle, sizeof(idle), &ev);
    CHECK(ev.frames == 1 && same_frame(&ev.last[0], HALTZ_MSG_ACK, p, 1));
    CHECK(ev.crc_errors >= 1);

    /* Long stream: random-ish noise containing many SYNCs, then a frame of every size. */
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    uint32_t x = 12345;
    for (int i = 0; i < 2000; i++) {
        x = x * 1103515245u + 12345u;
        uint8_t b = (uint8_t)(x >> 16);
        if ((i % 7) == 0) {
            b = HALTZ_SYNC;
        }
        on_result(&d, haltz_decoder_feed(&d, b), &ev);
    }
    feed_all(&d, idle, sizeof(idle), &ev); /* let any pending false frame time out */
    int before = ev.frames;
    uint8_t big[HALTZ_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7);
    }
    feed_all(&d, frame, n, &ev);
    uint8_t f2[HALTZ_MAX_FRAME];
    size_t n2 = make_frame(0x55, big, sizeof(big), f2);
    feed_all(&d, f2, n2, &ev);
    CHECK(ev.frames - before == 2);
}

static void test_bad_crc_rejected(void) {
    const uint8_t p[] = {0x01, 0x02, 0x03};
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = make_frame(0x10, p, sizeof(p), frame);

    /* Corrupt the CRC byte. */
    uint8_t bad[HALTZ_MAX_FRAME];
    memcpy(bad, frame, n);
    bad[n - 1] ^= 0x01;
    haltz_decoder_t d;
    haltz_decoder_init(&d);
    events_t ev = {0};
    feed_all(&d, bad, n, &ev);
    CHECK(ev.frames == 0 && ev.crc_errors == 1 && d.crc_errors == 1);

    /* Corrupt a payload byte. */
    memcpy(bad, frame, n);
    bad[4] ^= 0x80;
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    feed_all(&d, bad, n, &ev);
    CHECK(ev.frames == 0 && ev.crc_errors == 1);

    /* The next good frame still decodes. */
    feed_all(&d, frame, n, &ev);
    CHECK(ev.frames == 1 && same_frame(&ev.last[0], 0x10, p, sizeof(p)));
}

static void test_split_feeds(void) {
    uint8_t p[40];
    for (int i = 0; i < 40; i++) {
        p[i] = (uint8_t)(0xA0 + i); /* includes 0xA5 inside the payload */
    }
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = make_frame(0x20, p, sizeof(p), frame);

    haltz_decoder_t d;
    haltz_decoder_init(&d);
    events_t ev = {0};
    /* Chunks of 1, 2, 5, 13, rest; nothing must be reported before the last byte. */
    size_t cuts[] = {1, 3, 8, 21, n};
    size_t pos = 0;
    for (size_t c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
        feed_all(&d, frame + pos, cuts[c] - pos, &ev);
        pos = cuts[c];
        if (pos < n) {
            CHECK(ev.frames == 0);
        }
    }
    CHECK(ev.frames == 1 && ev.crc_errors == 0);
    CHECK(same_frame(&ev.last[0], 0x20, p, sizeof(p)));
}

static void test_len_limits(void) {
    haltz_decoder_t d;
    uint8_t frame[HALTZ_MAX_FRAME];

    /* LEN = 0 */
    size_t n = make_frame(0x30, NULL, 0, frame);
    CHECK(n == HALTZ_FRAME_OVERHEAD && frame[2] == 0);
    haltz_decoder_init(&d);
    events_t ev = {0};
    feed_all(&d, frame, n, &ev);
    CHECK(ev.frames == 1 && ev.last[0].type == 0x30 && ev.last[0].len == 0);

    /* LEN = 255 */
    uint8_t p[255];
    for (int i = 0; i < 255; i++) {
        p[i] = (uint8_t)(255 - i);
    }
    n = make_frame(0x31, p, 255, frame);
    CHECK(n == HALTZ_MAX_FRAME && frame[2] == 255);
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    feed_all(&d, frame, n, &ev);
    CHECK(ev.frames == 1 && same_frame(&ev.last[0], 0x31, p, 255));

    /* Back to back: 0, 255, 0 */
    haltz_decoder_init(&d);
    memset(&ev, 0, sizeof(ev));
    uint8_t f0[8];
    size_t n0 = 0;
    CHECK(haltz_frame_encode(0x30, NULL, 0, f0, sizeof(f0), &n0) == HALTZ_OK);
    feed_all(&d, f0, n0, &ev);
    feed_all(&d, frame, n, &ev);
    feed_all(&d, f0, n0, &ev);
    CHECK(ev.frames == 3 && ev.crc_errors == 0);
}

/* ---- GPU messages ------------------------------------------------------------------------ */

/* pack -> length check -> haltz_gpu_cmd_unpack -> same struct. Inputs must be memset to 0
 * before their fields are set, so padding and unused array tails compare equal. */
#define CHECK_GPU_ROUNDTRIP(name, id, in)                                                      \
    do {                                                                                       \
        uint8_t buf_[HALTZ_MAX_PAYLOAD];                                                       \
        size_t n_ = 0;                                                                         \
        static haltz_gpu_cmd_t out_;                                                           \
        memset(&out_, 0, sizeof(out_));                                                        \
        CHECK(haltz_gpu_##name##_pack(&(in), buf_, sizeof(buf_), &n_) == HALTZ_OK);            \
        CHECK(haltz_msg_check((id), n_) == HALTZ_OK);                                          \
        CHECK(haltz_gpu_cmd_unpack(&out_, (id), buf_, n_) == HALTZ_OK);                        \
        CHECK(out_.type == (id));                                                              \
        CHECK(memcmp(&out_.u.name, &(in), sizeof(in)) == 0);                                   \
    } while (0)

static void test_gpu_wire_layout(void) {
    uint8_t buf[HALTZ_MAX_PAYLOAD];
    size_t n = 0;

    haltz_gpu_sprite_set_t s;
    memset(&s, 0, sizeof(s));
    s.id = 5;
    s.x = -3;
    s.y = 300;
    s.pattern = 7;
    s.attr = HALTZ_GPU_SPR_VISIBLE | 1;
    CHECK(haltz_gpu_sprite_set_pack(&s, buf, sizeof(buf), &n) == HALTZ_OK);
    const uint8_t want_sprite[] = {0x05, 0xFD, 0xFF, 0x2C, 0x01, 0x07, 0x81};
    CHECK(n == sizeof(want_sprite) && memcmp(buf, want_sprite, n) == 0);

    haltz_gpu_palette_t p;
    memset(&p, 0, sizeof(p));
    p.first = 16;
    p.count = 2;
    p.colors[0] = 0xF800;
    p.colors[1] = 0x1234;
    CHECK(haltz_gpu_palette_pack(&p, buf, sizeof(buf), &n) == HALTZ_OK);
    const uint8_t want_palette[] = {0x10, 0x00, 0xF8, 0x34, 0x12};
    CHECK(n == sizeof(want_palette) && memcmp(buf, want_palette, n) == 0);

    haltz_gpu_text_scroll_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.top = 1;
    sc.height = 28;
    sc.rows = -2;
    sc.ch = ' ';
    sc.fg = 1;
    CHECK(haltz_gpu_text_scroll_pack(&sc, buf, sizeof(buf), &n) == HALTZ_OK);
    const uint8_t want_scroll[] = {0x01, 0x1C, 0xFE, 0x20, 0x01, 0x00};
    CHECK(n == sizeof(want_scroll) && memcmp(buf, want_scroll, n) == 0);

    haltz_gpu_status_t st;
    memset(&st, 0, sizeof(st));
    st.present_id = 0x0102;
    st.frames = 0x0304;
    st.render_us = 0x0506;
    st.rx_free_kb = 0x07;
    st.flags = HALTZ_GPU_STATUS_BUSY;
    st.crc_errors = 0x0908;
    st.cmd_errors = 0x0B0A;
    st.rx_overruns = 0x0D0C;
    st.uptime_ms = 0x11100F0Eu;
    CHECK(haltz_gpu_status_pack(&st, buf, sizeof(buf), &n) == HALTZ_OK);
    const uint8_t want_status[] = {0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x07, 0x01, 0x08,
                                   0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11};
    CHECK(n == HALTZ_MSG_GPU_STATUS_LEN && memcmp(buf, want_status, n) == 0);
    haltz_gpu_status_t st2;
    memset(&st2, 0, sizeof(st2));
    CHECK(haltz_gpu_status_unpack(&st2, buf, n) == HALTZ_OK);
    CHECK(memcmp(&st, &st2, sizeof(st)) == 0);
}

static void test_gpu_roundtrip_all(void) {
    static haltz_gpu_present_t present;
    present.id = 0xBEEF;
    CHECK_GPU_ROUNDTRIP(present, HALTZ_MSG_GPU_PRESENT, present);

    static haltz_gpu_backlight_t backlight;
    backlight.level = 200;
    CHECK_GPU_ROUNDTRIP(backlight, HALTZ_MSG_GPU_BACKLIGHT, backlight);

    static haltz_gpu_layers_t layers;
    layers.mask = HALTZ_GPU_LAYERS_ALL;
    CHECK_GPU_ROUNDTRIP(layers, HALTZ_MSG_GPU_LAYERS, layers);

    static haltz_gpu_palette_t palette; /* full-size message: 127 colours ending at 255 */
    palette.first = 129;
    palette.count = HALTZ_GPU_PALETTE_MAX;
    for (int i = 0; i < palette.count; i++) {
        palette.colors[i] = (uint16_t)(i * 517);
    }
    CHECK_GPU_ROUNDTRIP(palette, HALTZ_MSG_GPU_PALETTE, palette);

    static haltz_gpu_text_put_t text_put;
    text_put.col = 39;
    text_put.row = 29;
    text_put.fg = HALTZ_COLOR_YELLOW;
    text_put.bg = HALTZ_COLOR_DARK_BLUE;
    text_put.count = HALTZ_GPU_TEXT_PUT_MAX;
    for (int i = 0; i < text_put.count; i++) {
        text_put.chars[i] = (uint8_t)(255 - i);
    }
    CHECK_GPU_ROUNDTRIP(text_put, HALTZ_MSG_GPU_TEXT_PUT, text_put);

    static haltz_gpu_text_fill_t text_fill = {0, 0, 40, 30, ' ', 1, 0};
    CHECK_GPU_ROUNDTRIP(text_fill, HALTZ_MSG_GPU_TEXT_FILL, text_fill);

    static haltz_gpu_text_scroll_t text_scroll = {0, 29, 1, ' ', 1, 0};
    CHECK_GPU_ROUNDTRIP(text_scroll, HALTZ_MSG_GPU_TEXT_SCROLL, text_scroll);
    text_scroll.rows = -128;
    CHECK_GPU_ROUNDTRIP(text_scroll, HALTZ_MSG_GPU_TEXT_SCROLL, text_scroll);

    static haltz_gpu_text_cursor_t text_cursor = {10, 5,
                                                  HALTZ_GPU_CURSOR_BLOCK | HALTZ_GPU_CURSOR_BLINK};
    CHECK_GPU_ROUNDTRIP(text_cursor, HALTZ_MSG_GPU_TEXT_CURSOR, text_cursor);

    static haltz_gpu_font_load_t font_load; /* last 31 glyphs */
    font_load.first = 256 - HALTZ_GPU_FONT_LOAD_MAX;
    font_load.count = HALTZ_GPU_FONT_LOAD_MAX;
    memcpy(font_load.glyphs, haltz_gpu_default_font['A'], sizeof(font_load.glyphs));
    CHECK_GPU_ROUNDTRIP(font_load, HALTZ_MSG_GPU_FONT_LOAD, font_load);

    static haltz_gpu_pattern_load_t pattern_load;
    pattern_load.index = 255;
    for (int i = 0; i < HALTZ_GPU_PATTERN_BYTES; i++) {
        pattern_load.data[i] = (uint8_t)(i * 3);
    }
    CHECK_GPU_ROUNDTRIP(pattern_load, HALTZ_MSG_GPU_PATTERN_LOAD, pattern_load);

    static haltz_gpu_tile_put_t tile_put;
    tile_put.x = 31;
    tile_put.y = 31;
    tile_put.count = HALTZ_GPU_TILE_PUT_MAX;
    for (int i = 0; i < tile_put.count; i++) {
        tile_put.entries[i] = HALTZ_GPU_TILE(i, i & 15, (i & 1) ? HALTZ_GPU_TILE_HFLIP : 0);
    }
    CHECK_GPU_ROUNDTRIP(tile_put, HALTZ_MSG_GPU_TILE_PUT, tile_put);

    static haltz_gpu_tile_fill_t tile_fill = {0, 0, 32, 32, 0};
    tile_fill.entry = HALTZ_GPU_TILE(3, 2, HALTZ_GPU_TILE_HFLIP | HALTZ_GPU_TILE_VFLIP);
    CHECK_GPU_ROUNDTRIP(tile_fill, HALTZ_MSG_GPU_TILE_FILL, tile_fill);

    static haltz_gpu_tile_scroll_t tile_scroll = {511, 65535};
    CHECK_GPU_ROUNDTRIP(tile_scroll, HALTZ_MSG_GPU_TILE_SCROLL, tile_scroll);

    static haltz_gpu_sprite_set_t sprite_set;
    sprite_set.id = HALTZ_GPU_SPRITE_CURSOR;
    sprite_set.x = -32768;
    sprite_set.y = 32767;
    sprite_set.pattern = 9;
    sprite_set.attr = HALTZ_GPU_SPR_VISIBLE | HALTZ_GPU_SPR_ABOVE_TEXT | HALTZ_GPU_SPR_VFLIP;
    CHECK_GPU_ROUNDTRIP(sprite_set, HALTZ_MSG_GPU_SPRITE_SET, sprite_set);

    static haltz_gpu_bmp_pixels_t bmp_pixels;
    bmp_pixels.x = 300;
    bmp_pixels.y = 239;
    bmp_pixels.count = 1;
    bmp_pixels.pixels[0] = 42;
    CHECK_GPU_ROUNDTRIP(bmp_pixels, HALTZ_MSG_GPU_BMP_PIXELS, bmp_pixels);

    static haltz_gpu_bmp_fill_t bmp_fill;
    bmp_fill.x = 10;
    bmp_fill.y = 20;
    bmp_fill.w = 320;
    bmp_fill.h = 240;
    bmp_fill.color = 77;
    CHECK_GPU_ROUNDTRIP(bmp_fill, HALTZ_MSG_GPU_BMP_FILL, bmp_fill);

    static haltz_gpu_bmp_line_t bmp_line;
    bmp_line.x0 = -10;
    bmp_line.y0 = -20;
    bmp_line.x1 = 400;
    bmp_line.y1 = 300;
    bmp_line.color = 1;
    CHECK_GPU_ROUNDTRIP(bmp_line, HALTZ_MSG_GPU_BMP_LINE, bmp_line);

    static haltz_gpu_bmp_copy_t bmp_copy;
    bmp_copy.sx = 0;
    bmp_copy.sy = 8;
    bmp_copy.w = 320;
    bmp_copy.h = 232;
    bmp_copy.dx = 0;
    bmp_copy.dy = -1;
    CHECK_GPU_ROUNDTRIP(bmp_copy, HALTZ_MSG_GPU_BMP_COPY, bmp_copy);

    /* No-payload commands. */
    static haltz_gpu_cmd_t cmd;
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_INIT, NULL, 0) == HALTZ_OK);
    CHECK(cmd.type == HALTZ_MSG_GPU_INIT);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_STATUS_REQ, NULL, 0) == HALTZ_OK);
}

/* Through the real frame encoder and decoder, several commands back to back with filler. */
static void test_gpu_stream(void) {
    static uint8_t stream[1024];
    size_t len = 0;
    uint8_t payload[HALTZ_MAX_PAYLOAD];
    size_t n = 0;

    static haltz_gpu_text_put_t tp;
    memset(&tp, 0, sizeof(tp));
    tp.col = 2;
    tp.row = 3;
    tp.fg = HALTZ_COLOR_WHITE;
    tp.count = 5;
    memcpy(tp.chars, "HALTZ", 5);

    CHECK(haltz_frame_encode(HALTZ_MSG_GPU_INIT, NULL, 0, &stream[len], sizeof(stream) - len,
                             &n) == HALTZ_OK);
    len += n;
    stream[len++] = HALTZ_GPU_FILLER;
    stream[len++] = HALTZ_GPU_FILLER;
    CHECK(haltz_gpu_text_put_pack(&tp, payload, sizeof(payload), &n) == HALTZ_OK);
    CHECK(haltz_frame_encode(HALTZ_MSG_GPU_TEXT_PUT, payload, n, &stream[len],
                             sizeof(stream) - len, &n) == HALTZ_OK);
    len += n;
    haltz_gpu_present_t pr = {.id = 7};
    CHECK(haltz_gpu_present_pack(&pr, payload, sizeof(payload), &n) == HALTZ_OK);
    CHECK(haltz_frame_encode(HALTZ_MSG_GPU_PRESENT, payload, n, &stream[len],
                             sizeof(stream) - len, &n) == HALTZ_OK);
    len += n;

    static haltz_decoder_t d;
    haltz_decoder_init(&d);
    events_t ev = {0};
    feed_all(&d, stream, len, &ev);
    CHECK(ev.frames == 3 && ev.crc_errors == 0);

    static haltz_gpu_cmd_t cmd;
    CHECK(haltz_gpu_cmd_unpack(&cmd, ev.last[0].type, ev.last[0].payload, ev.last[0].len) ==
          HALTZ_OK);
    CHECK(cmd.type == HALTZ_MSG_GPU_INIT);
    CHECK(haltz_gpu_cmd_unpack(&cmd, ev.last[1].type, ev.last[1].payload, ev.last[1].len) ==
          HALTZ_OK);
    CHECK(cmd.type == HALTZ_MSG_GPU_TEXT_PUT && cmd.u.text_put.count == 5 &&
          memcmp(cmd.u.text_put.chars, "HALTZ", 5) == 0 && cmd.u.text_put.row == 3);
    CHECK(haltz_gpu_cmd_unpack(&cmd, ev.last[2].type, ev.last[2].payload, ev.last[2].len) ==
          HALTZ_OK);
    CHECK(cmd.type == HALTZ_MSG_GPU_PRESENT && cmd.u.present.id == 7);
}

static void test_gpu_bad_len(void) {
    static uint8_t z[HALTZ_MAX_PAYLOAD]; /* all zero */
    static haltz_gpu_cmd_t cmd;
    /* Outside the MIN..MAX range from msg_ids.def. */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_INIT, z, 1) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PRESENT, z, 3) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TEXT_PUT, z, 4) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PATTERN_LOAD, z, 128) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_BMP_PIXELS, z, 4) == HALTZ_ERR_BAD_LEN);
    /* Inside the range but the wrong shape. */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PALETTE, z, 4) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PALETTE, z, 5) == HALTZ_OK);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_FONT_LOAD, z, 10) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_FONT_LOAD, z, 17) == HALTZ_OK);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TILE_PUT, z, 5) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TILE_PUT, z, 6) == HALTZ_OK);
    /* Not S3 -> GPU commands. */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_HELLO, z, 7) == HALTZ_ERR_UNKNOWN_TYPE);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_STATUS, z, 18) == HALTZ_ERR_UNKNOWN_TYPE);
    CHECK(haltz_gpu_cmd_unpack(&cmd, 0x16, z, 0) == HALTZ_ERR_UNKNOWN_TYPE);
    CHECK(haltz_gpu_cmd_unpack(NULL, HALTZ_MSG_GPU_INIT, NULL, 0) == HALTZ_ERR_ARG);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PRESENT, NULL, 2) == HALTZ_ERR_ARG);
}

static void test_gpu_bad_value(void) {
    static haltz_gpu_cmd_t cmd;
    uint8_t buf[HALTZ_MAX_PAYLOAD];
    size_t n = 0;

    /* unpack side: raw payloads that break one rule each */
    const uint8_t layers[] = {0x10};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_LAYERS, layers, 1) == HALTZ_ERR_BAD_VALUE);
    const uint8_t palette[] = {255, 0, 0, 0, 0}; /* 2 colours from 255: past the end */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PALETTE, palette, 5) == HALTZ_ERR_BAD_VALUE);
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_PALETTE, palette, 3) == HALTZ_OK);
    const uint8_t text_col[] = {40, 0, 1, 0, 'x'};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TEXT_PUT, text_col, 5) ==
          HALTZ_ERR_BAD_VALUE);
    const uint8_t text_row[] = {0, 30, 1, 0, 'x'};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TEXT_PUT, text_row, 5) ==
          HALTZ_ERR_BAD_VALUE);
    const uint8_t cursor[] = {0, 0, 0x03};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TEXT_CURSOR, cursor, 3) ==
          HALTZ_ERR_BAD_VALUE);
    const uint8_t cursor_hidden[] = {200, 200, HALTZ_GPU_CURSOR_UNDERLINE}; /* off screen: ok */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TEXT_CURSOR, cursor_hidden, 3) == HALTZ_OK);
    static uint8_t font[1 + 2 * HALTZ_GPU_GLYPH_BYTES];
    font[0] = 255; /* 2 glyphs from 255 */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_FONT_LOAD, font, sizeof(font)) ==
          HALTZ_ERR_BAD_VALUE);
    const uint8_t tile_x[] = {32, 0, 0, 0};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TILE_PUT, tile_x, 4) == HALTZ_ERR_BAD_VALUE);
    const uint8_t tile_bits[] = {0, 0, 0x00, 0x00, 0x00, 0x40}; /* 2nd entry: reserved bit */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TILE_PUT, tile_bits, 6) ==
          HALTZ_ERR_BAD_VALUE);
    const uint8_t fill_bits[] = {0, 0, 1, 1, 0x00, 0x80};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TILE_FILL, fill_bits, 6) ==
          HALTZ_ERR_BAD_VALUE);
    const uint8_t fill_clip[] = {200, 200, 255, 255, 0x01, 0x00}; /* off the map: clipped */
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_TILE_FILL, fill_clip, 6) == HALTZ_OK);
    const uint8_t sprite[] = {64, 0, 0, 0, 0, 0, 0};
    CHECK(haltz_gpu_cmd_unpack(&cmd, HALTZ_MSG_GPU_SPRITE_SET, sprite, 7) ==
          HALTZ_ERR_BAD_VALUE);

    /* pack side refuses the same values before writing anything */
    haltz_gpu_layers_t l = {.mask = 0x80};
    CHECK(haltz_gpu_layers_pack(&l, buf, sizeof(buf), &n) == HALTZ_ERR_BAD_VALUE);
    static haltz_gpu_text_put_t tp;
    memset(&tp, 0, sizeof(tp));
    CHECK(haltz_gpu_text_put_pack(&tp, buf, sizeof(buf), &n) == HALTZ_ERR_BAD_VALUE); /* 0 chars */
    static haltz_gpu_palette_t pal;
    memset(&pal, 0, sizeof(pal));
    pal.count = HALTZ_GPU_PALETTE_MAX + 1;
    CHECK(haltz_gpu_palette_pack(&pal, buf, sizeof(buf), &n) == HALTZ_ERR_BAD_VALUE);
    static haltz_gpu_tile_put_t tput;
    memset(&tput, 0, sizeof(tput));
    tput.count = 2;
    tput.entries[1] = 0x8000;
    CHECK(haltz_gpu_tile_put_pack(&tput, buf, sizeof(buf), &n) == HALTZ_ERR_BAD_VALUE);
    haltz_gpu_sprite_set_t sp = {.id = 64};
    CHECK(haltz_gpu_sprite_set_pack(&sp, buf, sizeof(buf), &n) == HALTZ_ERR_BAD_VALUE);
    static haltz_gpu_bmp_pixels_t px;
    memset(&px, 0, sizeof(px));
    px.count = HALTZ_GPU_BMP_PIXELS_MAX + 1;
    CHECK(haltz_gpu_bmp_pixels_pack(&px, buf, sizeof(buf), &n) == HALTZ_ERR_BAD_VALUE);

    /* buffer too small, NULL arguments, out_len optional */
    haltz_gpu_present_t pr = {.id = 1};
    CHECK(haltz_gpu_present_pack(&pr, buf, 1, &n) == HALTZ_ERR_NO_SPACE);
    CHECK(haltz_gpu_present_pack(&pr, buf, 2, NULL) == HALTZ_OK);
    CHECK(haltz_gpu_present_pack(NULL, buf, 2, &n) == HALTZ_ERR_ARG);
    CHECK(haltz_gpu_present_pack(&pr, NULL, 2, &n) == HALTZ_ERR_ARG);
    tp.count = 10;
    CHECK(haltz_gpu_text_put_pack(&tp, buf, 13, &n) == HALTZ_ERR_NO_SPACE);
    CHECK(haltz_gpu_text_put_pack(&tp, buf, 14, &n) == HALTZ_OK && n == 14);
}

static int glyph_blank(const uint8_t *g) {
    for (int i = 0; i < HALTZ_GPU_GLYPH_BYTES; i++) {
        if (g[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void test_gpu_tables(void) {
    const uint16_t *pal = haltz_gpu_default_palette;
    CHECK(pal[HALTZ_COLOR_BACKDROP] == 0x0000);
    CHECK(pal[HALTZ_COLOR_WHITE] == 0xFFFF);
    CHECK(pal[HALTZ_COLOR_BLACK] == 0x0000);          /* cube 0,0,0 */
    CHECK(pal[HALTZ_COLOR_CUBE_FIRST + 215] == 0xFFFF); /* cube 5,5,5 */
    CHECK(pal[HALTZ_COLOR_CUBE_FIRST + 36 * 5] == 0xF800); /* cube 5,0,0 = pure red */
    CHECK(pal[HALTZ_COLOR_GREY_FIRST] == HALTZ_GPU_RGB565(8, 8, 8));
    CHECK(pal[255] == HALTZ_GPU_RGB565(238, 238, 238));
    /* System colours are all different and none is black. */
    for (int i = 1; i <= 15; i++) {
        CHECK(pal[i] != 0);
        for (int j = 1; j < i; j++) {
            CHECK(pal[i] != pal[j]);
        }
    }

    const uint8_t (*font)[HALTZ_GPU_GLYPH_BYTES] = haltz_gpu_default_font;
    for (int c = 0; c < 0x20; c++) {
        CHECK(glyph_blank(font[c]));
    }
    CHECK(glyph_blank(font[' ']) && glyph_blank(font[0x7F]));
    int ascii_ok = 1;
    for (int c = 0x21; c <= 0x7E; c++) {
        /* Visible, unique, and inside columns 1-5 so neighbours never touch. */
        ascii_ok &= !glyph_blank(font[c]);
        for (int r = 0; r < HALTZ_GPU_GLYPH_BYTES; r++) {
            ascii_ok &= (font[c][r] & 0x83) == 0;
        }
        for (int d = 0x21; d < c; d++) {
            ascii_ok &= memcmp(font[c], font[d], HALTZ_GPU_GLYPH_BYTES) != 0;
        }
    }
    CHECK(ascii_ok);
    CHECK(font[HALTZ_GLYPH_BOX_H][3] == 0xFF);
    CHECK(font[HALTZ_GLYPH_BOX_V][0] == 0x10 && font[HALTZ_GLYPH_BOX_V][7] == 0x10);
    CHECK(font[HALTZ_GLYPH_BOX_CROSS][3] == 0xFF && font[HALTZ_GLYPH_BOX_CROSS][0] == 0x10);
    CHECK(font[HALTZ_GLYPH_BLOCK_FULL][0] == 0xFF && font[HALTZ_GLYPH_BLOCK_FULL][7] == 0xFF);
    CHECK(font[HALTZ_GLYPH_BLOCK_LEFT][4] == 0xF0 && font[HALTZ_GLYPH_BLOCK_LOWER][3] == 0x00);
    CHECK(font[HALTZ_GLYPH_SHADE_MEDIUM][0] == 0xAA && font[HALTZ_GLYPH_SHADE_MEDIUM][1] == 0x55);
    for (int c = HALTZ_GLYPH_SHADE_DARK + 1; c < 256; c++) {
        CHECK(glyph_blank(font[c]));
    }
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"crc8 test vector", test_crc8_vector},
        {"message tables", test_msg_tables},
        {"GPU message table and constants", test_gpu_msg_table},
        {"HELLO pack/unpack round trip", test_hello_roundtrip},
        {"ACK/NAK pack/unpack round trip", test_ack_nak_roundtrip},
        {"encode then decode", test_encode_then_decode},
        {"decoder resync after garbage", test_resync_after_garbage},
        {"bad CRC rejected", test_bad_crc_rejected},
        {"frame split across feeds", test_split_feeds},
        {"LEN = 0 and LEN = 255", test_len_limits},
        {"GPU wire layout (bytes)", test_gpu_wire_layout},
        {"GPU pack/unpack round trip, every command", test_gpu_roundtrip_all},
        {"GPU commands through encoder and decoder", test_gpu_stream},
        {"GPU bad lengths and types", test_gpu_bad_len},
        {"GPU bad values, buffer space, arguments", test_gpu_bad_value},
        {"GPU default palette and font", test_gpu_tables},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
