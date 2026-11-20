/*
 * HALTZ GPU (Pico 2 W, RP2350) - step 1: boot self-test over stdio (UART0 -> debug probe).
 * Prints the banner and a HELLO encode/decode self-test, then idles.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "board_pins.h"
#include "haltz/board_ids.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"
#include "haltz/proto.h"

#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

static haltz_decoder_t g_dec; /* ~800 bytes, keep it off the stack */

static bool hello_self_test(void) {
    haltz_hello_t hello = {.board_id = HALTZ_BOARD_GPU, .proto_version = HALTZ_PROTO_VERSION};
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

    haltz_decoder_init(&g_dec);
    bool got = false;
    haltz_hello_t back = {0};
    for (size_t i = 0; i < frame_len; i++) {
        haltz_dec_result_t r = haltz_decoder_feed(&g_dec, frame[i]);
        for (; r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&g_dec)) {
            const haltz_frame_t *f = haltz_decoder_frame(&g_dec);
            if (r == HALTZ_DEC_FRAME && f->type == HALTZ_MSG_HELLO &&
                haltz_hello_unpack(&back, f->payload, f->len) == HALTZ_OK) {
                got = true;
            }
        }
    }
    return got && back.board_id == hello.board_id &&
           back.proto_version == hello.proto_version &&
           memcmp(back.git_hash, hello.git_hash, sizeof(back.git_hash)) == 0;
}

int main(void) {
    stdio_init_all(); /* UART0 on GP0/GP1 (PIN_STDIO_UART_TX/RX), 115200 8N1 */
    sleep_ms(200);    /* let the probe's UART settle */

    printf("\n==========================================\n");
    printf(" HALTZ GPU\n");
    printf(" protocol v%u, git %s\n", (unsigned)HALTZ_PROTO_VERSION, HALTZ_GIT_HASH);
    printf("==========================================\n");

    bool ok = hello_self_test();
    printf("HELLO encode/decode: %s\n", ok ? "OK" : "FAIL");
    printf("idle.\n");

    while (true) {
        __wfi(); /* nothing to do yet: sleep until an interrupt */
    }
}
