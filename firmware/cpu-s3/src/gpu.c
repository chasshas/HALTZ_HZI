/*
 * HALTZ CPU: GPU link driver (docs/gpu-protocol-v1.md sections 1, 2 and 7). See gpu.h.
 *
 * One mutex (s_lock) covers the shadow, the two DMA buffers and the SPI device, so a command
 * changes the shadow and enters the byte stream in the same order. Whoever holds the lock may
 * start a transaction:
 *   - a drawing call whose frame doesn't fit the buffer being filled sends that buffer,
 *   - gpu_present() sends at once,
 *   - the driver task sends a buffer 2 ms after its first frame, and clocks 64-byte polls
 *     while a reply is due.
 * At most one transaction is in flight: DMA sends one buffer while the other one fills.
 * Before each transaction BUSY must be low (or drop within 200 ms, else the GPU is reset).
 *
 * MISO: every byte of every transaction goes into one decoder, never flushed at CS (the GPU
 * may split a frame across transactions). HELLO, GPU_STATUS and NAK are handled there.
 *
 * Health (section 2): no GPU_STATUS for 1 s, BUSY high for 200 ms, or no FRAME pulse 500 ms
 * after a PRESENT -> hard reset, handshake, GPU_INIT, replay of the shadow, PRESENT.
 * An unrequested HELLO or GPU_STATUS.uptime_ms going backwards -> the GPU rebooted by itself:
 * handshake (no reset), then the same replay.
 */
#include "gpu.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "haltz/board_ids.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"

/* gpu_state.h is the GPU firmware's picture state (portable C, no Pico SDK calls): the shadow
 * is changed by the same code that changes the GPU's own copy, so the two can't drift apart. */
#include "gpu_replay.h"

#if defined(__has_include)
#if __has_include("haltz_git_hash.h")
#include "haltz_git_hash.h"
#endif
#endif
#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

#define BUF_SIZE         HALTZ_GPU_MAX_XFER /* one transaction */
#define FLUSH_AFTER_US   2000  /* send a buffer this long after its first frame */
#define STATUS_EVERY_MS  250   /* GPU_STATUS_REQ (protocol: at least every 500 ms) */
#define POLL_EVERY_US    2000  /* filler poll while a reply is due and nothing else goes out */
#define HELLO_POLL_MS    10    /* filler poll between two HELLOs */
#define HELLO_GRACE_MS   300   /* HELLOs this soon after link up answer our repeated ones */
#define MISSING_RETRY_MS 10000 /* GPU missing: try again this often */
#define RESET_HOLD_MS    5     /* RUN low; protocol minimum is HALTZ_GPU_RESET_MS */
#define LOG_EVERY_MS     1000  /* GPU counter warnings at most this often */

#define TASK_STACK 6144
#define TASK_PRIO  8 /* above the kernel tasks: replies and health checks stay on time */
#define TASK_CORE  0 /* architecture-v1: system services on core 0, the VM on core 1 */

#define EV_FRAME BIT0 /* a FRAME pulse */
#define EV_LINK  BIT1 /* the link state changed */

/* ---- state ------------------------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static spi_device_handle_t s_dev;
static uint32_t s_spi_hz;

/* Buffers (internal RAM, DMA). s_fill is being filled, the other one may be in flight. */
static uint8_t *s_tx[2];
static uint8_t *s_rx[2];
static spi_transaction_t s_trans[2];
static int s_fill;
static size_t s_fill_len;
static int64_t s_fill_since_us;
static bool s_fill_has_present;
static int s_inflight = -1; /* buffer of the queued transaction, or -1 */
static int64_t s_last_xfer_us;

static gpu_state_t *s_shadow; /* PSRAM */
static uint8_t s_backlight = 255; /* not part of the picture state; the GPU boots at 255 */
static haltz_decoder_t s_dec;
static haltz_gpu_cmd_t s_rc; /* replay scratch (driver task, under the lock) */

/* Link state. s_state is read without the lock by gpu_is_up() and the waiters. */
static volatile gpu_link_state_t s_state = GPU_LINK_RESET;
static int64_t s_state_since_ms;
static int s_tries;     /* resets in the current attempt to bring the link up */
static bool s_failed;   /* link_fail() ran; cleared by the reset */
static const char *s_reason;
static int64_t s_next_hello_ms, s_next_poll_ms, s_retry_ms;
/* Handshake: the GPU's HELLO alone isn't enough. It queues its boot HELLO before its panel is
 * set up (~0.5 s of ST7789 init delays) and reads no commands until that is done, so a PRESENT
 * sent then would miss the 500 ms FRAME timeout. A GPU_STATUS answer proves its command loop
 * runs: only then the replay. */
static bool s_hello_seen;
static bool s_hs_status;      /* GPU_STATUS received during the handshake */
static int64_t s_hs_hello_ms; /* when the HELLO came; the STATUS wait is timed from here */
static bool s_reboot_seen;
static int64_t s_up_since_ms;
static uint16_t s_peer_proto;
static uint8_t s_peer_git[4];

static bool s_status_due; /* a GPU_STATUS_REQ is unanswered */
static int64_t s_next_status_ms, s_last_status_ms;
static bool s_have_status;
static haltz_gpu_status_t s_status;
static int64_t s_busy_since_ms;
static int64_t s_last_warn_ms;

/* PRESENT pacing. The counters are shared with the FRAME interrupt: s_count_mux. */
static portMUX_TYPE s_count_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_present_id;
static volatile uint32_t s_presents;     /* PRESENTs emitted since link up */
static volatile uint32_t s_frame_pulses; /* all FRAME pulses since boot (ISR) */
static volatile uint32_t s_frames_base;  /* s_frame_pulses at link up */
static int64_t s_present_sent_ms;        /* transaction with the last PRESENT started; 0 = not yet */

/* Statistics */
static uint32_t s_resets, s_link_ups, s_naks, s_miso_crc_errors, s_busy_waits, s_transactions;
static uint64_t s_tx_bytes;

static int64_t now_us(void) {
    return esp_timer_get_time();
}

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

/* ---- FRAME pin and pacing ---------------------------------------------------------------- */

static void frame_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&s_count_mux);
    s_frame_pulses++;
    portEXIT_CRITICAL_ISR(&s_count_mux);
    BaseType_t woken = pdFALSE;
    xEventGroupSetBitsFromISR(s_events, EV_FRAME, &woken);
    portYIELD_FROM_ISR(woken);
}

/* True while some PRESENT sent since link up has not been shown yet. */
static bool present_in_flight(void) {
    portENTER_CRITICAL(&s_count_mux);
    uint32_t shown = s_frame_pulses - s_frames_base;
    bool pending = (int32_t)(s_presents - shown) > 0;
    portEXIT_CRITICAL(&s_count_mux);
    return pending;
}

/* Sleep until a FRAME pulse, a link change, or ms (the caller re-checks its condition: with
 * several waiters one may clear the bit before another sees it, so slices stay short). */
static void wait_event(uint32_t ms) {
    TickType_t t = pdMS_TO_TICKS(ms);
    xEventGroupWaitBits(s_events, EV_FRAME | EV_LINK, pdTRUE, pdFALSE, t > 0 ? t : 1);
}

static void enter_state(gpu_link_state_t st) {
    s_state = st;
    s_state_since_ms = now_ms();
    s_next_hello_ms = s_state_since_ms;
    s_next_poll_ms = s_state_since_ms;
    if (st == GPU_LINK_HANDSHAKE) {
        s_hello_seen = false;
        s_hs_status = false;
        s_hs_hello_ms = 0;
    }
    xEventGroupSetBits(s_events, EV_LINK);
}

/* ---- sending (all under the lock) -------------------------------------------------------- */

static void discard_fill(void) {
    s_fill_len = 0;
    s_fill_has_present = false;
}

static void handle_rx(const haltz_frame_t *f);

static void feed_rx(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        for (haltz_dec_result_t r = haltz_decoder_feed(&s_dec, p[i]); r != HALTZ_DEC_NEED_MORE;
             r = haltz_decoder_poll(&s_dec)) {
            if (r == HALTZ_DEC_FRAME) {
                handle_rx(haltz_decoder_frame(&s_dec));
            } else {
                s_miso_crc_errors++;
            }
        }
    }
}

/* Collect the transaction in flight (if any) and decode what came back on MISO. Without
 * block it returns at once when the transaction is still running. */
static void finish_inflight(bool block) {
    if (s_inflight < 0) {
        return;
    }
    spi_transaction_t *done = NULL;
    if (spi_device_get_trans_result(s_dev, &done, block ? portMAX_DELAY : 0) != ESP_OK) {
        return; /* still running */
    }
    int b = s_inflight;
    s_inflight = -1;
    s_last_xfer_us = now_us();
    feed_rx(s_rx[b], done->length / 8);
}

static void link_fail(const char *why);

/* BUSY must be low before a transaction starts (never checked during one). */
static bool wait_not_busy(void) {
    if (!gpio_get_level(PIN_GPU_BUSY)) {
        return true;
    }
    s_busy_waits++;
    int64_t t0 = now_ms();
    while (gpio_get_level(PIN_GPU_BUSY)) {
        if (now_ms() - t0 > HALTZ_GPU_BUSY_STUCK_MS) {
            link_fail("BUSY high for more than 200 ms");
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/* Send the buffer being filled (if it holds anything) and switch to the other one. */
static bool kick(bool check_busy) {
    if (s_failed) {
        return false;
    }
    if (s_fill_len == 0) {
        return true;
    }
    finish_inflight(true); /* frees the other buffer */
    if (check_busy && !wait_not_busy()) {
        return false;
    }
    /* Whole words, so the driver can DMA straight into the RX buffer (filler is allowed
     * between frames). */
    size_t len = (s_fill_len + 3u) & ~(size_t)3u;
    memset(s_tx[s_fill] + s_fill_len, HALTZ_GPU_FILLER, len - s_fill_len);
    spi_transaction_t *t = &s_trans[s_fill];
    memset(t, 0, sizeof(*t));
    t->length = len * 8;
    t->tx_buffer = s_tx[s_fill];
    t->rx_buffer = s_rx[s_fill];
    esp_err_t err = spi_device_queue_trans(s_dev, t, portMAX_DELAY);
    if (err != ESP_OK) {
        printf("gpu: spi_device_queue_trans: %s\n", esp_err_to_name(err));
        link_fail("SPI driver error");
        return false;
    }
    s_inflight = s_fill;
    s_transactions++;
    s_tx_bytes += len;
    if (s_fill_has_present) {
        s_present_sent_ms = now_ms();
    }
    s_fill ^= 1;
    discard_fill();
    return true;
}

/* Append one frame. False if the link failed (the frame is dropped; the replay after the
 * reset sends the shadow anyway). */
static bool emit(uint8_t type, const uint8_t *payload, size_t len) {
    if (s_failed) {
        return false;
    }
    if (s_fill_len + len + HALTZ_FRAME_OVERHEAD > BUF_SIZE && !kick(true)) {
        return false;
    }
    size_t n = 0;
    if (haltz_frame_encode(type, payload, len, s_tx[s_fill] + s_fill_len, BUF_SIZE - s_fill_len,
                           &n) != HALTZ_OK) {
        return false;
    }
    if (s_fill_len == 0) {
        s_fill_since_us = now_us();
    }
    s_fill_len += n;
    return true;
}

static void emit_cmd(const haltz_gpu_cmd_t *c, void *ctx) {
    (void)ctx;
    uint8_t p[HALTZ_MAX_PAYLOAD];
    size_t n = 0;
    if (haltz_gpu_cmd_pack(c, p, sizeof(p), &n) == HALTZ_OK) {
        emit(c->type, p, n);
    }
}

/* Make the buffer being filled at least n bytes long with filler: a poll. */
static void pad_fill(size_t n) {
    if (s_fill_len >= n) {
        return;
    }
    if (s_fill_len == 0) {
        s_fill_since_us = now_us();
    }
    memset(s_tx[s_fill] + s_fill_len, HALTZ_GPU_FILLER, n - s_fill_len);
    s_fill_len = n;
}

static void emit_present(void) {
    haltz_gpu_present_t pr = {.id = (uint16_t)(s_present_id + 1)};
    uint8_t p[HALTZ_MSG_GPU_PRESENT_LEN];
    haltz_gpu_present_pack(&pr, p, sizeof(p), NULL);
    if (emit(HALTZ_MSG_GPU_PRESENT, p, sizeof(p))) {
        s_present_id = pr.id;
        portENTER_CRITICAL(&s_count_mux);
        s_presents++;
        portEXIT_CRITICAL(&s_count_mux);
        s_present_sent_ms = 0; /* set when its transaction starts */
        s_fill_has_present = true;
    }
}

static void send_hello(void) {
    haltz_hello_t h = {.board_id = HALTZ_BOARD_CPU, .proto_version = HALTZ_PROTO_VERSION};
    haltz_git_hash_parse(HALTZ_GIT_HASH, h.git_hash);
    uint8_t p[HALTZ_MSG_HELLO_LEN];
    haltz_hello_pack(&h, p, sizeof(p));
    emit(HALTZ_MSG_HELLO, p, sizeof(p));
}

/* Something is wrong with the GPU: stop sending, let the driver task reset it. */
static void link_fail(const char *why) {
    if (s_failed) {
        return;
    }
    s_failed = true;
    s_reason = why;
    discard_fill();
    if (s_state == GPU_LINK_UP) {
        s_tries = 0;
    }
    enter_state(GPU_LINK_RESET);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* ---- receiving (under the lock) ---------------------------------------------------------- */

static void handle_status(const haltz_gpu_status_t *st) {
    int64_t now = now_ms();
    if (s_state == GPU_LINK_HANDSHAKE && s_hello_seen) {
        s_hs_status = true;
    }
    if (s_state == GPU_LINK_UP && s_have_status) {
        if (st->uptime_ms < s_status.uptime_ms) {
            s_reboot_seen = true; /* uptime went backwards */
        }
        bool overrun = st->rx_overruns != s_status.rx_overruns;
        bool crc = st->crc_errors != s_status.crc_errors;
        if ((overrun || crc) && now - s_last_warn_ms >= LOG_EVERY_MS) {
            s_last_warn_ms = now;
            printf("gpu: GPU counters: crc errors %u, receive overruns %u%s\n",
                   (unsigned)st->crc_errors, (unsigned)st->rx_overruns,
                   overrun ? " (overrun: data lost, the S3 ignored BUSY?)"
                           : " (bad frames on MOSI: noise? lower the SPI clock)");
        }
    }
    s_status = *st;
    s_have_status = true;
    s_last_status_ms = now;
    s_status_due = false;
}

static void handle_rx(const haltz_frame_t *f) {
    switch (f->type) {
    case HALTZ_MSG_HELLO: {
        haltz_hello_t h;
        if (haltz_hello_unpack(&h, f->payload, f->len) != HALTZ_OK) {
            return;
        }
        s_peer_proto = h.proto_version;
        memcpy(s_peer_git, h.git_hash, sizeof(s_peer_git));
        if (s_state == GPU_LINK_HANDSHAKE) {
            s_hello_seen = true;
        } else if (s_state == GPU_LINK_UP && now_ms() - s_up_since_ms > HELLO_GRACE_MS) {
            s_reboot_seen = true; /* a HELLO nobody asked for: the GPU booted */
        }
        return;
    }
    case HALTZ_MSG_GPU_STATUS: {
        haltz_gpu_status_t st;
        if (haltz_gpu_status_unpack(&st, f->payload, f->len) == HALTZ_OK) {
            handle_status(&st);
        }
        return;
    }
    case HALTZ_MSG_NAK: {
        haltz_nak_t nak;
        if (haltz_nak_unpack(&nak, f->payload, f->len) != HALTZ_OK) {
            return;
        }
        s_naks++;
        if (s_naks <= 8 || s_naks % 256 == 0) {
            printf("gpu: NAK for %s (0x%02X): error %u (%" PRIu32 " so far)\n",
                   haltz_msg_name(nak.type), nak.type, nak.error, s_naks);
        }
        return;
    }
    default:
        printf("gpu: unexpected %s (0x%02X) from the GPU\n", haltz_msg_name(f->type), f->type);
        return;
    }
}

/* ---- driver task ------------------------------------------------------------------------- */

static void do_reset(void) {
    finish_inflight(true);
    discard_fill();
    printf("gpu: %s%sresetting the GPU (try %d of %d)\n", s_reason ? s_reason : "",
           s_reason ? ": " : "", s_tries + 1, HALTZ_GPU_HELLO_TRIES);
    gpio_set_level(PIN_GPU_PICO_RUN, 0);
    vTaskDelay(pdMS_TO_TICKS(RESET_HOLD_MS));
    gpio_set_level(PIN_GPU_PICO_RUN, 1);
    s_resets++;
    s_tries++;
    s_failed = false;
    s_hello_seen = false;
    haltz_decoder_init(&s_dec);
    enter_state(GPU_LINK_HANDSHAKE);
}

static void bring_up(void) {
    s_hello_seen = false;
    char git[9];
    haltz_git_hash_format(s_peer_git, git);
    printf("gpu: HELLO from the GPU: protocol v%u, git %s\n", (unsigned)s_peer_proto, git);
    if (s_peer_proto != HALTZ_PROTO_VERSION) {
        printf("gpu: WARNING: the GPU speaks protocol v%u, this CPU v%u. Re-flash both boards "
               "from the same commit. Carrying on.\n",
               (unsigned)s_peer_proto, (unsigned)HALTZ_PROTO_VERSION);
    }

    int64_t now = now_ms();
    discard_fill();
    portENTER_CRITICAL(&s_count_mux);
    s_frames_base = s_frame_pulses;
    s_presents = 0;
    portEXIT_CRITICAL(&s_count_mux);
    s_have_status = false;
    s_status_due = false;
    s_last_status_ms = now;
    s_next_status_ms = now;
    s_busy_since_ms = 0;
    s_reboot_seen = false;

    /* Our HELLO once more: the GPU's HELLO may be its boot HELLO, sent before it could hear
     * ours (it only counts the CPU as there after a HELLO). Then the whole picture, and
     * show it. The answer to this HELLO comes during the replay or within the grace time. */
    uint64_t bytes0 = s_tx_bytes;
    int64_t t0 = now_us();
    send_hello();
    emit(HALTZ_MSG_GPU_INIT, NULL, 0);
    gpu_replay(s_shadow, &s_rc, emit_cmd, NULL);
    if (s_backlight != 255) {
        haltz_gpu_backlight_t bl = {.level = s_backlight};
        uint8_t p[HALTZ_MSG_GPU_BACKLIGHT_LEN];
        haltz_gpu_backlight_pack(&bl, p, sizeof(p), NULL);
        emit(HALTZ_MSG_GPU_BACKLIGHT, p, sizeof(p));
    }
    emit_present();
    kick(true);
    if (s_failed) {
        return; /* already back in RESET */
    }
    s_link_ups++;
    /* Timers from now: a long replay (up to ~100 KB) must not count as GPU silence. */
    s_up_since_ms = now_ms();
    s_last_status_ms = s_up_since_ms;
    s_next_status_ms = s_up_since_ms;
    enter_state(GPU_LINK_UP);
    printf("gpu: link up at %" PRIu32 " Hz, picture sent (%u bytes in %u ms)\n", s_spi_hz,
           (unsigned)(s_tx_bytes - bytes0), (unsigned)((now_us() - t0) / 1000));
}

static void do_handshake(void) {
    int64_t now = now_ms();
    if (s_hello_seen && s_hs_status) {
        bring_up();
        return;
    }
    if (s_hello_seen && s_hs_hello_ms == 0) {
        s_hs_hello_ms = now; /* phase 2: GPU_STATUS_REQ until the command loop answers */
        s_next_hello_ms = now;
    }
    int64_t waited = s_hello_seen ? now - s_hs_hello_ms : now - s_state_since_ms;
    if (waited >= HALTZ_GPU_HELLO_TIMEOUT_MS) {
        const char *what = s_hello_seen ? "HELLO, but no GPU_STATUS within 2 s"
                                        : "no HELLO within 2 s";
        if (s_tries >= HALTZ_GPU_HELLO_TRIES) {
            printf("gpu: GPU MISSING (%s) after %d resets. Is Pico #1 powered and running "
                   "haltz_gpu.uf2 (not the wiring test)? Trying again every %d s.\n",
                   what, s_tries, MISSING_RETRY_MS / 1000);
            enter_state(GPU_LINK_MISSING);
            s_retry_ms = now + MISSING_RETRY_MS;
        } else {
            s_reason = what;
            enter_state(GPU_LINK_RESET);
        }
        return;
    }
    /* BUSY is not valid yet (it floats during the GPU's boot; the pull-down reads it low), but
     * a running GPU firmware holds it high until it is ready: then just try later. */
    if (s_inflight >= 0 || gpio_get_level(PIN_GPU_BUSY)) {
        return;
    }
    if (now >= s_next_hello_ms) {
        s_next_hello_ms = now + HALTZ_GPU_HELLO_EVERY_MS;
        s_next_poll_ms = now + HELLO_POLL_MS;
        if (s_hello_seen) {
            emit(HALTZ_MSG_GPU_STATUS_REQ, NULL, 0);
        } else {
            send_hello();
        }
        pad_fill(HALTZ_GPU_POLL_XFER); /* HELLO or STATUS_REQ + filler, 64 bytes */
        kick(false);
    } else if (now >= s_next_poll_ms) {
        s_next_poll_ms = now + HELLO_POLL_MS;
        pad_fill(HALTZ_GPU_POLL_XFER);
        kick(false);
    }
}

static void do_up(void) {
    int64_t now = now_ms();
    if (s_reboot_seen) {
        s_reboot_seen = false;
        s_reason = "the GPU restarted by itself";
        printf("gpu: %s; sending the picture again\n", s_reason);
        discard_fill();
        s_tries = 0;
        s_hello_seen = false;
        enter_state(GPU_LINK_HANDSHAKE); /* our HELLO, then the replay; reset if no answer */
        return;
    }

    if (s_fill_len > 0 && now_us() - s_fill_since_us >= FLUSH_AFTER_US) {
        kick(true);
    }
    if (now >= s_next_status_ms) {
        s_next_status_ms = now + STATUS_EVERY_MS;
        s_status_due = true;
        emit(HALTZ_MSG_GPU_STATUS_REQ, NULL, 0); /* goes out with the next flush */
    }
    if (s_status_due && s_inflight < 0 && s_fill_len == 0 &&
        now_us() - s_last_xfer_us >= POLL_EVERY_US) {
        pad_fill(HALTZ_GPU_POLL_XFER);
        kick(true);
    }

    /* Health */
    if (gpio_get_level(PIN_GPU_BUSY)) {
        if (s_busy_since_ms == 0) {
            s_busy_since_ms = now;
        } else if (now - s_busy_since_ms > HALTZ_GPU_BUSY_STUCK_MS) {
            link_fail("BUSY high for more than 200 ms");
        }
    } else {
        s_busy_since_ms = 0;
    }
    if (now - s_last_status_ms > HALTZ_GPU_DEAD_MS) {
        link_fail("no GPU_STATUS for 1 s");
    }
    if (s_present_sent_ms != 0 && present_in_flight() &&
        now - s_present_sent_ms > HALTZ_GPU_FRAME_TIMEOUT_MS) {
        link_fail("no FRAME pulse 500 ms after PRESENT");
    }
}

static void gpu_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, 1); /* every tick (1 ms), or at once after link_fail */
        lock();
        finish_inflight(false);
        switch (s_state) {
        case GPU_LINK_RESET:
            do_reset();
            break;
        case GPU_LINK_HANDSHAKE:
            do_handshake();
            break;
        case GPU_LINK_UP:
            do_up();
            break;
        case GPU_LINK_MISSING:
            if (now_ms() >= s_retry_ms) {
                s_tries = 0;
                s_reason = NULL;
                enter_state(GPU_LINK_RESET);
            }
            break;
        }
        unlock();
    }
}

/* ---- start, status ----------------------------------------------------------------------- */

static esp_err_t add_device(uint32_t hz) {
    spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)hz,
        .mode = HALTZ_GPU_SPI_MODE,
        .spics_io_num = PIN_GPU_CS,
        .queue_size = 2,
    };
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (err == ESP_OK) {
        int khz = 0;
        spi_device_get_actual_freq(s_dev, &khz);
        s_spi_hz = (uint32_t)khz * 1000u;
    }
    return err;
}

esp_err_t gpu_start(void) {
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_shadow = heap_caps_malloc(sizeof(gpu_state_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 2; i++) {
        s_tx[i] = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_rx[i] = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_tx[i] == NULL || s_rx[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_shadow == NULL || s_lock == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    gpu_state_init(s_shadow);
    haltz_decoder_init(&s_dec);

    /* RESET: open drain, released (the Pico's RUN pin has its own pull-up). */
    gpio_set_level(PIN_GPU_PICO_RUN, 1);
    gpio_config_t run = {.pin_bit_mask = 1ull << PIN_GPU_PICO_RUN,
                         .mode = GPIO_MODE_OUTPUT_OD};
    gpio_config_t in = {.pin_bit_mask = (1ull << PIN_GPU_BUSY) | (1ull << PIN_GPU_FRAME),
                        .mode = GPIO_MODE_INPUT,
                        .pull_down_en = GPIO_PULLDOWN_ENABLE}; /* both float while the Pico boots */
    esp_err_t err = gpio_config(&run);
    if (err == ESP_OK) {
        err = gpio_config(&in);
    }
    if (err == ESP_OK) {
        err = gpio_set_intr_type(PIN_GPU_FRAME, GPIO_INTR_POSEDGE);
    }
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK; /* someone installed it already */
        }
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(PIN_GPU_FRAME, frame_isr, NULL);
    }
    if (err != ESP_OK) {
        printf("gpu: pin setup failed: %s\n", esp_err_to_name(err));
        return err;
    }

    spi_bus_config_t bus = {.mosi_io_num = PIN_GPU_MOSI,
                            .miso_io_num = PIN_GPU_MISO,
                            .sclk_io_num = PIN_GPU_SCK,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = BUF_SIZE};
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        err = add_device(GPU_SPI_HZ_BOOT);
    }
    if (err != ESP_OK) {
        printf("gpu: SPI host A setup failed: %s\n", esp_err_to_name(err));
        return err;
    }
    gpio_pulldown_en(PIN_GPU_MISO); /* an open MISO reads 0x00 = filler, never SYNC */

    s_tries = 0;
    s_reason = NULL;
    enter_state(GPU_LINK_RESET);
    if (xTaskCreatePinnedToCore(gpu_task, "gpu", TASK_STACK, NULL, TASK_PRIO, &s_task,
                                TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool gpu_is_up(void) {
    return s_state == GPU_LINK_UP;
}

bool gpu_wait_up(uint32_t timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    while (s_lock != NULL && !gpu_is_up()) {
        if (now_ms() >= end) {
            return false;
        }
        wait_event(10);
    }
    return s_lock != NULL;
}

const char *gpu_link_state_name(gpu_link_state_t s) {
    switch (s) {
    case GPU_LINK_RESET: return "reset";
    case GPU_LINK_HANDSHAKE: return "handshake";
    case GPU_LINK_UP: return "up";
    case GPU_LINK_MISSING: return "missing";
    default: return "?";
    }
}

void gpu_get_stats(gpu_stats_t *out) {
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    lock();
    out->state = s_state;
    out->spi_hz = s_spi_hz;
    out->peer_proto = s_peer_proto;
    haltz_git_hash_format(s_peer_git, out->peer_git);
    out->resets = s_resets;
    out->link_ups = s_link_ups;
    out->last_reset_reason = s_reason;
    portENTER_CRITICAL(&s_count_mux);
    out->presents = s_presents;
    out->frames = s_frame_pulses - s_frames_base;
    portEXIT_CRITICAL(&s_count_mux);
    out->naks = s_naks;
    out->miso_crc_errors = s_miso_crc_errors;
    out->busy_waits = s_busy_waits;
    out->transactions = s_transactions;
    out->tx_bytes = s_tx_bytes;
    out->have_status = s_have_status;
    out->status = s_status;
    out->status_age_ms = s_have_status ? (uint32_t)(now_ms() - s_last_status_ms) : 0;
    unlock();
}

esp_err_t gpu_set_spi_hz(uint32_t hz) {
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hz < 1000000 || hz > HALTZ_GPU_SPI_HZ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    kick(true); /* what is buffered goes out at the old speed */
    finish_inflight(true);
    uint32_t old = s_spi_hz;
    esp_err_t err = spi_bus_remove_device(s_dev);
    if (err == ESP_OK) {
        err = add_device(hz);
        if (err != ESP_OK) {
            add_device(old); /* keep a working device */
        }
    }
    unlock();
    if (err == ESP_OK) {
        printf("gpu: SPI clock now %" PRIu32 " Hz\n", s_spi_hz);
    }
    return err;
}

void gpu_force_reset(void) {
    if (s_lock == NULL) {
        return;
    }
    lock();
    s_tries = 0;
    if (s_state == GPU_LINK_UP || s_state == GPU_LINK_HANDSHAKE) {
        link_fail("reset requested");
    } else {
        s_reason = "reset requested";
        enter_state(GPU_LINK_RESET);
    }
    unlock();
}

/* ---- frames ------------------------------------------------------------------------------ */

uint16_t gpu_present(void) {
    if (s_lock == NULL) {
        return 0;
    }
    /* Pacing: the next PRESENT only after the previous one's FRAME pulse. If that never comes
     * the driver resets the GPU (state leaves UP) and the replay brings the picture back. */
    int64_t end = now_ms() + HALTZ_GPU_FRAME_TIMEOUT_MS + 200;
    for (;;) {
        lock();
        if (s_state != GPU_LINK_UP) {
            uint16_t id = s_present_id;
            unlock();
            return id;
        }
        if (!present_in_flight()) {
            break; /* keep the lock */
        }
        unlock();
        if (now_ms() >= end) {
            return s_present_id; /* the driver is about to reset the GPU */
        }
        wait_event(2);
    }
    emit_present();
    kick(true);
    uint16_t id = s_present_id;
    unlock();
    return id;
}

bool gpu_vsync(uint32_t timeout_ms) {
    if (s_lock == NULL || !gpu_is_up()) {
        vTaskDelay(pdMS_TO_TICKS(16));
        return false;
    }
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        if (!gpu_is_up()) {
            return false;
        }
        if (!present_in_flight()) {
            return true;
        }
        if (now_ms() >= end) {
            return false;
        }
        wait_event(2);
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */

static haltz_err_t submit(const haltz_gpu_cmd_t *cmd) {
    uint8_t payload[HALTZ_MAX_PAYLOAD];
    size_t n = 0;
    haltz_err_t err = haltz_gpu_cmd_pack(cmd, payload, sizeof(payload), &n);
    if (err != HALTZ_OK) {
        return err;
    }
    if (s_lock == NULL) {
        return HALTZ_ERR_BUSY; /* gpu_start() not called */
    }
    lock();
    if (cmd->type == HALTZ_MSG_GPU_BACKLIGHT) {
        s_backlight = cmd->u.backlight.level;
    } else {
        gpu_state_apply(s_shadow, cmd);
    }
    if (s_state == GPU_LINK_UP) {
        emit(cmd->type, payload, n);
    }
    unlock();
    return HALTZ_OK;
}

haltz_err_t gpu_cmd(const haltz_gpu_cmd_t *cmd) {
    if (cmd == NULL || cmd->type == HALTZ_MSG_GPU_PRESENT ||
        cmd->type == HALTZ_MSG_GPU_STATUS_REQ) {
        return HALTZ_ERR_ARG;
    }
    return submit(cmd);
}

haltz_err_t gpu_init_picture(void) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_INIT};
    return submit(&c);
}

haltz_err_t gpu_backlight(uint8_t level) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_BACKLIGHT};
    c.u.backlight.level = level;
    return submit(&c);
}

haltz_err_t gpu_layers(uint8_t mask) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_LAYERS};
    c.u.layers.mask = mask;
    return submit(&c);
}

haltz_err_t gpu_palette(uint8_t first, const uint16_t *colors, size_t n) {
    if (n == 0) {
        return HALTZ_OK;
    }
    if (colors == NULL) {
        return HALTZ_ERR_ARG;
    }
    if (first + n > HALTZ_GPU_PALETTE_SIZE) {
        return HALTZ_ERR_BAD_VALUE;
    }
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_PALETTE};
    while (n > 0) {
        size_t k = n < HALTZ_GPU_PALETTE_MAX ? n : HALTZ_GPU_PALETTE_MAX;
        c.u.palette.first = first;
        c.u.palette.count = (uint8_t)k;
        memcpy(c.u.palette.colors, colors, k * sizeof(uint16_t));
        haltz_err_t err = submit(&c);
        if (err != HALTZ_OK) {
            return err;
        }
        first = (uint8_t)(first + k);
        colors += k;
        n -= k;
    }
    return HALTZ_OK;
}

haltz_err_t gpu_text_put(uint8_t col, uint8_t row, uint8_t fg, uint8_t bg, const uint8_t *chars,
                         size_t n) {
    if (n == 0) {
        return HALTZ_OK;
    }
    if (chars == NULL) {
        return HALTZ_ERR_ARG;
    }
    if (col >= HALTZ_GPU_TEXT_COLS || row >= HALTZ_GPU_TEXT_ROWS) {
        return HALTZ_ERR_BAD_VALUE;
    }
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TEXT_PUT};
    size_t pos = (size_t)row * HALTZ_GPU_TEXT_COLS + col;
    const size_t end = (size_t)HALTZ_GPU_TEXT_COLS * HALTZ_GPU_TEXT_ROWS;
    while (n > 0 && pos < end) {
        size_t k = n < HALTZ_GPU_TEXT_PUT_MAX ? n : HALTZ_GPU_TEXT_PUT_MAX;
        c.u.text_put.col = (uint8_t)(pos % HALTZ_GPU_TEXT_COLS);
        c.u.text_put.row = (uint8_t)(pos / HALTZ_GPU_TEXT_COLS);
        c.u.text_put.fg = fg;
        c.u.text_put.bg = bg;
        c.u.text_put.count = (uint8_t)k;
        memcpy(c.u.text_put.chars, chars, k);
        haltz_err_t err = submit(&c);
        if (err != HALTZ_OK) {
            return err;
        }
        pos += k;
        chars += k;
        n -= k;
    }
    return HALTZ_OK;
}

haltz_err_t gpu_text_print(uint8_t col, uint8_t row, uint8_t fg, uint8_t bg, const char *s) {
    if (s == NULL) {
        return HALTZ_ERR_ARG;
    }
    return gpu_text_put(col, row, fg, bg, (const uint8_t *)s, strlen(s));
}

haltz_err_t gpu_text_fill(uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t ch, uint8_t fg,
                          uint8_t bg) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TEXT_FILL};
    c.u.text_fill = (haltz_gpu_text_fill_t){col, row, w, h, ch, fg, bg};
    return submit(&c);
}

haltz_err_t gpu_text_scroll(uint8_t top, uint8_t height, int8_t rows, uint8_t ch, uint8_t fg,
                            uint8_t bg) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TEXT_SCROLL};
    c.u.text_scroll = (haltz_gpu_text_scroll_t){top, height, rows, ch, fg, bg};
    return submit(&c);
}

haltz_err_t gpu_text_cursor(uint8_t col, uint8_t row, uint8_t style) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TEXT_CURSOR};
    c.u.text_cursor = (haltz_gpu_text_cursor_t){col, row, style};
    return submit(&c);
}

haltz_err_t gpu_font_load(uint8_t first, const uint8_t (*glyphs)[HALTZ_GPU_GLYPH_BYTES],
                          size_t n) {
    if (n == 0) {
        return HALTZ_OK;
    }
    if (glyphs == NULL) {
        return HALTZ_ERR_ARG;
    }
    if (first + n > HALTZ_GPU_FONT_GLYPHS) {
        return HALTZ_ERR_BAD_VALUE;
    }
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_FONT_LOAD};
    while (n > 0) {
        size_t k = n < HALTZ_GPU_FONT_LOAD_MAX ? n : HALTZ_GPU_FONT_LOAD_MAX;
        c.u.font_load.first = first;
        c.u.font_load.count = (uint8_t)k;
        memcpy(c.u.font_load.glyphs, glyphs, k * HALTZ_GPU_GLYPH_BYTES);
        haltz_err_t err = submit(&c);
        if (err != HALTZ_OK) {
            return err;
        }
        first = (uint8_t)(first + k);
        glyphs += k;
        n -= k;
    }
    return HALTZ_OK;
}

haltz_err_t gpu_pattern_load(uint8_t index, const uint8_t data[HALTZ_GPU_PATTERN_BYTES]) {
    if (data == NULL) {
        return HALTZ_ERR_ARG;
    }
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_PATTERN_LOAD};
    c.u.pattern_load.index = index;
    memcpy(c.u.pattern_load.data, data, HALTZ_GPU_PATTERN_BYTES);
    return submit(&c);
}

haltz_err_t gpu_tile_put(uint8_t x, uint8_t y, const uint16_t *entries, size_t n) {
    if (n == 0) {
        return HALTZ_OK;
    }
    if (entries == NULL) {
        return HALTZ_ERR_ARG;
    }
    if (x >= HALTZ_GPU_MAP_W || y >= HALTZ_GPU_MAP_H) {
        return HALTZ_ERR_BAD_VALUE;
    }
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TILE_PUT};
    size_t pos = (size_t)y * HALTZ_GPU_MAP_W + x;
    const size_t end = (size_t)HALTZ_GPU_MAP_W * HALTZ_GPU_MAP_H;
    while (n > 0 && pos < end) {
        size_t k = n < HALTZ_GPU_TILE_PUT_MAX ? n : HALTZ_GPU_TILE_PUT_MAX;
        c.u.tile_put.x = (uint8_t)(pos % HALTZ_GPU_MAP_W);
        c.u.tile_put.y = (uint8_t)(pos / HALTZ_GPU_MAP_W);
        c.u.tile_put.count = (uint8_t)k;
        memcpy(c.u.tile_put.entries, entries, k * sizeof(uint16_t));
        haltz_err_t err = submit(&c);
        if (err != HALTZ_OK) {
            return err;
        }
        pos += k;
        entries += k;
        n -= k;
    }
    return HALTZ_OK;
}

haltz_err_t gpu_tile_fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t entry) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TILE_FILL};
    c.u.tile_fill = (haltz_gpu_tile_fill_t){x, y, w, h, entry};
    return submit(&c);
}

haltz_err_t gpu_tile_scroll(uint16_t sx, uint16_t sy) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TILE_SCROLL};
    c.u.tile_scroll = (haltz_gpu_tile_scroll_t){sx, sy};
    return submit(&c);
}

haltz_err_t gpu_sprite_set(uint8_t id, int16_t x, int16_t y, uint8_t pattern, uint8_t attr) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_SPRITE_SET};
    c.u.sprite_set = (haltz_gpu_sprite_set_t){id, x, y, pattern, attr};
    return submit(&c);
}

haltz_err_t gpu_bmp_pixels(uint16_t x, uint16_t y, const uint8_t *pixels, size_t n) {
    if (n == 0) {
        return HALTZ_OK;
    }
    if (pixels == NULL) {
        return HALTZ_ERR_ARG;
    }
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_BMP_PIXELS};
    while (n > 0 && x < HALTZ_GPU_SCREEN_W) { /* the rest would be clipped anyway */
        size_t k = n < HALTZ_GPU_BMP_PIXELS_MAX ? n : HALTZ_GPU_BMP_PIXELS_MAX;
        c.u.bmp_pixels.x = x;
        c.u.bmp_pixels.y = y;
        c.u.bmp_pixels.count = (uint8_t)k;
        memcpy(c.u.bmp_pixels.pixels, pixels, k);
        haltz_err_t err = submit(&c);
        if (err != HALTZ_OK) {
            return err;
        }
        x = (uint16_t)(x + k);
        pixels += k;
        n -= k;
    }
    return HALTZ_OK;
}

haltz_err_t gpu_bmp_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_BMP_FILL};
    c.u.bmp_fill = (haltz_gpu_bmp_fill_t){x, y, w, h, color};
    return submit(&c);
}

haltz_err_t gpu_bmp_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_BMP_LINE};
    c.u.bmp_line = (haltz_gpu_bmp_line_t){x0, y0, x1, y1, color};
    return submit(&c);
}

haltz_err_t gpu_bmp_copy(uint16_t sx, uint16_t sy, uint16_t w, uint16_t h, int16_t dx,
                         int16_t dy) {
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_BMP_COPY};
    c.u.bmp_copy = (haltz_gpu_bmp_copy_t){sx, sy, w, h, dx, dy};
    return submit(&c);
}
