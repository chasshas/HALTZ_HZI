// HALTZ I/O board: boot banner and HELLO encode/decode self-test.
// C++ on purpose: it checks that the shared C headers work from C++ (extern "C" guards).
#include <cstdio>
#include <cstring>

#include "board_pins.h"
#include "haltz/board_ids.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"
#include "haltz/proto.h"

#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

static haltz_decoder_t s_dec; // ~800 bytes, keep it off the stack

static bool hello_self_test() {
    haltz_hello_t hello = {};
    hello.board_id = HALTZ_BOARD_IO;
    hello.proto_version = HALTZ_PROTO_VERSION;
    haltz_git_hash_parse(HALTZ_GIT_HASH, hello.git_hash);

    uint8_t payload[HALTZ_MSG_HELLO_LEN];
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t frame_len = 0;
    if (haltz_hello_pack(&hello, payload, sizeof(payload)) != HALTZ_OK ||
        haltz_frame_encode(HALTZ_MSG_HELLO, payload, sizeof(payload), frame, sizeof(frame),
                           &frame_len) != HALTZ_OK) {
        return false;
    }

    printf("HELLO frame (%u bytes):", (unsigned)frame_len);
    for (size_t i = 0; i < frame_len; i++) {
        printf(" %02X", frame[i]);
    }
    printf("\n");

    haltz_decoder_init(&s_dec);
    bool got = false;
    haltz_hello_t back = {};
    for (size_t i = 0; i < frame_len; i++) {
        haltz_dec_result_t r = haltz_decoder_feed(&s_dec, frame[i]);
        for (; r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&s_dec)) {
            const haltz_frame_t *f = haltz_decoder_frame(&s_dec);
            if (r == HALTZ_DEC_FRAME && f->type == HALTZ_MSG_HELLO &&
                haltz_hello_unpack(&back, f->payload, f->len) == HALTZ_OK) {
                got = true;
            }
        }
    }
    return got && back.board_id == hello.board_id &&
           back.proto_version == hello.proto_version &&
           std::memcmp(back.git_hash, hello.git_hash, sizeof(back.git_hash)) == 0;
}

// Called from app_main() in main.c before Bluepad32 starts.
extern "C" void haltz_io_selftest(void) {
    printf("\n==========================================\n");
    printf(" HALTZ IO\n");
    printf(" protocol v%u, git %s\n", (unsigned)HALTZ_PROTO_VERSION, HALTZ_GIT_HASH);
    printf("==========================================\n");
    printf("HELLO encode/decode: %s\n", hello_self_test() ? "OK" : "FAIL");
}
