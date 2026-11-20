/*
 * HALTZ CPU (ESP32-S3) - step 1: boot self-test.
 *
 * ---- Memory rules for this board ---------------------------------------------------------
 *  - Plain malloc() stays in internal RAM. PSRAM only through
 *    heap_caps_malloc(..., MALLOC_CAP_SPIRAM).
 *  - DMA buffers and task stacks always live in internal RAM.
 *  - Never use GPIO 35, 36, 37: they are the octal PSRAM bus.
 *  - Do not add -mfix-esp32-psram-cache-issue or -D CONFIG_... build flags.
 *    All configuration goes in sdkconfig.defaults.
 * -------------------------------------------------------------------------------------------
 *
 * Self-test, printed on the console:
 *  1. Banner: "HALTZ CPU", protocol version, git hash, ESP-IDF version.
 *  2. Chip info, flash size, PSRAM size, free internal and PSRAM heap.
 *  3. HELLO frame encode -> decode with haltz_proto.
 *  4. XIP test: a task on core 1 hammers a PSRAM buffer while core 0 erases and writes
 *     sectors of the "storage" partition. With code executing from PSRAM
 *     (CONFIG_SPIRAM_XIP_FROM_PSRAM) core 1 must keep running during the flash writes.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "haltz/board_ids.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"
#include "haltz/proto.h"

#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

/* ---- 1 + 2: banner and system info ------------------------------------------------------ */

static void print_banner(void) {
    printf("\n");
    printf("==========================================\n");
    printf(" HALTZ CPU\n");
    printf(" protocol v%u, git %s, ESP-IDF %s\n", (unsigned)HALTZ_PROTO_VERSION, HALTZ_GIT_HASH,
           esp_get_idf_version());
    printf("==========================================\n");
}

static void print_system_info(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("chip: %s rev v%d.%d, %d cores, features:%s%s%s%s\n",
           CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores,
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? " WiFi" : "",
           (chip.features & CHIP_FEATURE_BLE) ? " BLE" : "",
           (chip.features & CHIP_FEATURE_EMB_FLASH) ? " emb-flash" : "",
           (chip.features & CHIP_FEATURE_EMB_PSRAM) ? " emb-psram" : "");

    uint32_t flash_cfg = 0;
    uint32_t flash_phys = 0;
    esp_flash_get_size(NULL, &flash_cfg);
    esp_flash_get_physical_size(NULL, &flash_phys);
    printf("flash: %" PRIu32 " MB configured, %" PRIu32 " MB detected %s\n",
           flash_cfg / (1024 * 1024), flash_phys / (1024 * 1024),
           (flash_phys == 16u * 1024 * 1024) ? "(OK)" : "(expected 16 MB!)");

    size_t psram = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    printf("psram: %u MB %s\n", (unsigned)(psram / (1024 * 1024)),
           (psram == 8u * 1024 * 1024) ? "(OK)" : "(expected 8 MB!)");

    printf("heap: internal free %u bytes (largest block %u), psram free %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- 3: HELLO encode/decode ------------------------------------------------------------- */

static bool hello_self_test(void) {
    haltz_hello_t hello = {.board_id = HALTZ_BOARD_CPU, .proto_version = HALTZ_PROTO_VERSION};
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

    static haltz_decoder_t dec; /* ~800 bytes: keep it off the stack */
    haltz_decoder_init(&dec);
    bool got = false;
    haltz_hello_t back = {0};
    for (size_t i = 0; i < frame_len; i++) {
        haltz_dec_result_t r = haltz_decoder_feed(&dec, frame[i]);
        for (; r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&dec)) {
            const haltz_frame_t *f = haltz_decoder_frame(&dec);
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

/* ---- 4: XIP test ------------------------------------------------------------------------ */

#define XIP_PSRAM_BUF_BYTES (256 * 1024) /* bigger than the data cache: real PSRAM traffic */
#define XIP_SECTORS         4
#define XIP_ROUNDS          10
#define XIP_SECTOR_SIZE     4096
#define XIP_YIELD_US        100000 /* worker sleeps 1 tick every 100 ms (idle task / WDT) */

typedef struct {
    uint32_t *buf; /* PSRAM */
    size_t words;
    volatile uint32_t iterations;
    volatile int64_t max_gap_us; /* longest time between two iterations while measuring */
    volatile bool measuring;
    volatile bool stop;
    volatile bool done;
    volatile uint64_t sum_check; /* sum of the buffer at the end, must equal iterations */
} xip_ctx_t;

/* Runs on core 1 from normal (non-IRAM) code, i.e. executing from PSRAM via XIP. */
static void psram_worker(void *arg) {
    xip_ctx_t *c = (xip_ctx_t *)arg;
    uint32_t i = 0;
    int64_t last = esp_timer_get_time();
    int64_t last_yield = last;
    while (!c->stop) {
        /* Stride 64 bytes so each step touches a new cache line. Every iteration adds 1 to
         * one word, so at the end the buffer must sum to the iteration count. */
        size_t idx = ((size_t)i * 16u) % c->words;
        c->buf[idx] = c->buf[idx] + 1u;
        i++;
        c->iterations = i;

        int64_t now = esp_timer_get_time();
        if (c->measuring && (now - last) > c->max_gap_us) {
            c->max_gap_us = now - last;
        }
        last = now;
        if (now - last_yield > XIP_YIELD_US) {
            vTaskDelay(1);
            last = last_yield = esp_timer_get_time(); /* voluntary sleep is not a stall */
        }
    }
    uint64_t sum = 0;
    for (size_t w = 0; w < c->words; w++) {
        sum += c->buf[w];
    }
    c->sum_check = sum;
    c->done = true;
    vTaskDelete(NULL);
}

static bool xip_test(void) {
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part == NULL || part->size < XIP_SECTORS * XIP_SECTOR_SIZE) {
        printf("XIP: no 'storage' partition\n");
        return false;
    }
    printf("XIP: storage partition at 0x%06" PRIx32 ", %" PRIu32 " KB\n", part->address,
           part->size / 1024);

    static xip_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.words = XIP_PSRAM_BUF_BYTES / sizeof(uint32_t);
    ctx.buf = heap_caps_calloc(ctx.words, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    uint8_t *wbuf = heap_caps_malloc(XIP_SECTOR_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *rbuf = heap_caps_malloc(XIP_SECTOR_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ctx.buf == NULL || wbuf == NULL || rbuf == NULL) {
        printf("XIP: allocation failed (psram buf %p)\n", (void *)ctx.buf);
        heap_caps_free(ctx.buf);
        heap_caps_free(wbuf);
        heap_caps_free(rbuf);
        return false;
    }

    /* Stack in internal RAM (default for xTaskCreate). Same priority as app_main. */
    if (xTaskCreatePinnedToCore(psram_worker, "xip_psram", 4096, &ctx, 5, NULL, 1) != pdPASS) {
        printf("XIP: cannot start worker\n");
        return false;
    }

    /* Baseline speed without flash activity. */
    vTaskDelay(pdMS_TO_TICKS(50));
    uint32_t b0 = ctx.iterations;
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(200));
    uint32_t b1 = ctx.iterations;
    int64_t t1 = esp_timer_get_time();
    double base_rate = (double)(b1 - b0) * 1e6 / (double)(t1 - t0);

    ctx.max_gap_us = 0;
    ctx.measuring = true;
    bool ok = true;
    int ops = 0;
    int ops_progressed = 0;
    int64_t min_erase_us = INT64_MAX;
    int64_t max_erase_us = 0;
    uint32_t f0 = ctx.iterations;
    int64_t tf0 = esp_timer_get_time();

    for (int round = 0; round < XIP_ROUNDS && ok; round++) {
        for (int s = 0; s < XIP_SECTORS && ok; s++) {
            size_t off = (size_t)s * XIP_SECTOR_SIZE;
            for (int k = 0; k < XIP_SECTOR_SIZE; k++) {
                wbuf[k] = (uint8_t)(k * 31 + round * 7 + s);
            }

            uint32_t c0 = ctx.iterations;
            int64_t e0 = esp_timer_get_time();
            esp_err_t err = esp_partition_erase_range(part, off, XIP_SECTOR_SIZE);
            int64_t e1 = esp_timer_get_time();
            uint32_t c1 = ctx.iterations;
            if (err == ESP_OK) {
                err = esp_partition_write(part, off, wbuf, XIP_SECTOR_SIZE);
            }
            uint32_t c2 = ctx.iterations;
            if (err == ESP_OK) {
                err = esp_partition_read(part, off, rbuf, XIP_SECTOR_SIZE);
            }
            if (err != ESP_OK || memcmp(wbuf, rbuf, XIP_SECTOR_SIZE) != 0) {
                printf("XIP: flash op failed round %d sector %d: %s\n", round, s,
                       err != ESP_OK ? esp_err_to_name(err) : "readback mismatch");
                ok = false;
                break;
            }
            ops += 2;
            ops_progressed += (c1 != c0) + (c2 != c1);
            int64_t et = e1 - e0;
            min_erase_us = et < min_erase_us ? et : min_erase_us;
            max_erase_us = et > max_erase_us ? et : max_erase_us;
        }
    }

    uint32_t f1 = ctx.iterations;
    int64_t tf1 = esp_timer_get_time();
    ctx.measuring = false;
    ctx.stop = true;
    while (!ctx.done) {
        vTaskDelay(1);
    }
    double flash_rate = (double)(f1 - f0) * 1e6 / (double)(tf1 - tf0);
    bool sum_ok = ctx.sum_check == ctx.iterations;

    printf("XIP: %d erase+write ops (%d rounds x %d sectors), erase %lld..%lld us\n", ops,
           XIP_ROUNDS, XIP_SECTORS, (long long)min_erase_us, (long long)max_erase_us);
    printf("XIP: core-1 rate %.0f it/s idle, %.0f it/s during flash writes\n", base_rate,
           flash_rate);
    printf("XIP: core-1 progressed during %d/%d ops, longest stall %lld us, psram sum %s\n",
           ops_progressed, ops, (long long)ctx.max_gap_us, sum_ok ? "OK" : "MISMATCH");

    /* PASS: flash ops worked, core 1 advanced during every single op, never stalled for
     * as long as half an erase, and the PSRAM data is intact. */
    bool pass = ok && ops > 0 && ops_progressed == ops && min_erase_us != INT64_MAX &&
                ctx.max_gap_us < min_erase_us / 2 && sum_ok;

    heap_caps_free(ctx.buf);
    heap_caps_free(wbuf);
    heap_caps_free(rbuf);
    return pass;
}

void app_main(void) {
    print_banner();
    print_system_info();

    bool hello_ok = hello_self_test();
    printf("HELLO encode/decode: %s\n", hello_ok ? "OK" : "FAIL");

    bool xip_ok = xip_test();
    printf("XIP test: %s\n", xip_ok ? "PASS" : "FAIL");

    printf("self-test done: %s. heap: internal free %u, psram free %u\n",
           (hello_ok && xip_ok) ? "ALL OK" : "FAILURES",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    (void)PIN_HALT_BUTTON; /* pins are defined but unused in step 1 */

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
