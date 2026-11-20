/* PC unit tests for haltz_proto. No framework: each CHECK prints on failure; exit code = fails. */
#include <stdio.h>
#include <string.h>

#include "haltz/board_ids.h"
#include "haltz/crc8.h"
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
    CHECK(HALTZ_MSG_COUNT == 3);
    CHECK(HALTZ_MSG_HELLO == 0x01 && HALTZ_MSG_ACK == 0x02 && HALTZ_MSG_NAK == 0x03);
    CHECK(HALTZ_MSG_HELLO_LEN == 7 && HALTZ_MSG_ACK_LEN == 1 && HALTZ_MSG_NAK_LEN == 2);
    CHECK(strcmp(haltz_msg_name(0x01), "HELLO") == 0);
    CHECK(strcmp(haltz_msg_name(0x03), "NAK") == 0);
    CHECK(strcmp(haltz_msg_name(0x7E), "?") == 0);
    CHECK(haltz_msg_payload_len(0x02) == 1);
    CHECK(haltz_msg_payload_len(0x00) == -1);
    CHECK(haltz_msg_check(HALTZ_MSG_HELLO, 7) == HALTZ_OK);
    CHECK(haltz_msg_check(HALTZ_MSG_HELLO, 6) == HALTZ_ERR_BAD_LEN);
    CHECK(haltz_msg_check(0xEE, 0) == HALTZ_ERR_UNKNOWN_TYPE);
    CHECK(HALTZ_BOARD_CPU == 1 && HALTZ_BOARD_IO == 2 && HALTZ_BOARD_GPU == 3);
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

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"crc8 test vector", test_crc8_vector},
        {"message tables", test_msg_tables},
        {"HELLO pack/unpack round trip", test_hello_roundtrip},
        {"ACK/NAK pack/unpack round trip", test_ack_nak_roundtrip},
        {"encode then decode", test_encode_then_decode},
        {"decoder resync after garbage", test_resync_after_garbage},
        {"bad CRC rejected", test_bad_crc_rejected},
        {"frame split across feeds", test_split_feeds},
        {"LEN = 0 and LEN = 255", test_len_limits},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
