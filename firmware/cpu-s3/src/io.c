/*
 * HALTZ CPU: I/O link driver (docs/io-protocol-v1.md sections 1, 2 and 9). See io.h.
 *
 * The driver task owns UART1 RX: UART driver events -> decoder (flushed after 10 ms of
 * silence inside a frame) -> handle_rx(). Frames go out from any task through send_frame()
 * under s_lock, so they never interleave. s_lock also covers the kept state, the input state
 * and the statistics.
 *
 * Handshake: EN low 20 ms, release, HELLO every 100 ms until the board's HELLO (3 s, then
 * reset again; 3 tries, then MISSING with a new try every 10 s). Link up: send the kept state.
 * Health: nothing received for 1.5 s -> reset. An unrequested HELLO (more than 300 ms after
 * link up) or IO_STATUS.uptime_ms going backwards -> the board rebooted: send the kept state
 * again, no reset.
 */
#include "io.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "haltz/board_ids.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"

#if defined(__has_include)
#if __has_include("haltz_git_hash.h")
#include "haltz_git_hash.h"
#endif
#endif
#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

#define UART_NUM         UART_NUM_1
#define UART_RX_BUF      2048
#define UART_TX_BUF      2048
#define UART_QUEUE_LEN   16
#define EVENT_QUEUE_LEN  64
#define TICK_MS          10    /* the task wakes at least this often */
#define HELLO_GRACE_MS   300   /* HELLOs this soon after link up answer our repeated ones */
#define MISSING_RETRY_MS 10000
#define LOG_EVERY_MS     1000  /* NAK / bad frame warnings at most this often */

#define TASK_STACK 4096
#define TASK_PRIO  7 /* below the GPU driver (8), above the kernel tasks */
#define TASK_CORE  0 /* architecture-v1: system services on core 0 */

/* ---- state ------------------------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_uart_q;
static QueueHandle_t s_events;
static haltz_decoder_t s_dec; /* task only */

static volatile io_link_state_t s_state = IO_LINK_RESET; /* changed by the task only */
static volatile bool s_reset_req; /* io_force_reset() */
static int s_tries;
static int64_t s_hs_start_ms, s_next_hello_ms, s_retry_ms, s_up_since_ms;
static int64_t s_last_rx_ms;
static bool s_hello_seen;
static const char *s_reason;
static uint16_t s_peer_proto;
static uint8_t s_peer_git[4];

/* Kept state, sent at every link up (under s_lock). */
static bool s_keep_led_valid;
static haltz_io_led_t s_keep_led;
static uint8_t s_keep_volume[HALTZ_IO_VOICES] = {HALTZ_IO_VOLUME_FULL, HALTZ_IO_VOLUME_FULL};
static bool s_keep_scan = true;

/* Input (under s_lock) */
static io_input_t s_in;
static uint8_t s_dev_kind[HALTZ_IO_MAX_DEVICES];

/* Status and statistics (under s_lock) */
static bool s_have_status;
static haltz_io_status_t s_status;
static int64_t s_status_ms;
static uint32_t s_resets, s_link_ups, s_reboots;
static uint32_t s_rx_frames, s_rx_crc, s_rx_bad, s_naks, s_tx_frames, s_dropped;
static uint64_t s_tx_bytes;
static int64_t s_last_warn_ms;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

/* ---- sending (caller holds s_lock) ------------------------------------------------------- */

static void send_frame(uint8_t type, const uint8_t *payload, size_t len) {
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = 0;
    if (haltz_frame_encode(type, payload, len, frame, sizeof(frame), &n) != HALTZ_OK) {
        return;
    }
    uart_write_bytes(UART_NUM, frame, n); /* one piece: no gap inside the frame */
    s_tx_frames++;
    s_tx_bytes += n;
}

static void send_packed(const haltz_io_cmd_t *c) {
    uint8_t p[HALTZ_MAX_PAYLOAD];
    size_t n = 0;
    if (haltz_io_cmd_pack(c, p, sizeof(p), &n) == HALTZ_OK) {
        send_frame(c->type, p, n);
    }
}

static void send_hello(void) {
    haltz_hello_t h = {.board_id = HALTZ_BOARD_CPU, .proto_version = HALTZ_PROTO_VERSION};
    haltz_git_hash_parse(HALTZ_GIT_HASH, h.git_hash);
    uint8_t p[HALTZ_MSG_HELLO_LEN];
    if (haltz_hello_pack(&h, p, sizeof(p)) == HALTZ_OK) {
        send_frame(HALTZ_MSG_HELLO, p, sizeof(p));
    }
}

static void send_kept_state(void) {
    haltz_io_cmd_t c;
    if (s_keep_led_valid) {
        c.type = HALTZ_MSG_IO_LED;
        c.u.led = s_keep_led;
        send_packed(&c);
    }
    for (int v = 0; v < HALTZ_IO_VOICES; v++) {
        if (s_keep_volume[v] != HALTZ_IO_VOLUME_FULL) {
            c.type = HALTZ_MSG_IO_VOLUME;
            c.u.volume = (haltz_io_volume_t){.voices = (uint8_t)(1u << v), .level = s_keep_volume[v]};
            send_packed(&c);
        }
    }
    if (!s_keep_scan) {
        c.type = HALTZ_MSG_IO_BT;
        c.u.bt.action = HALTZ_IO_BT_SCAN_OFF;
        send_packed(&c);
    }
}

/* ---- events (caller holds s_lock) -------------------------------------------------------- */

static void post(const io_event_t *ev) {
    if (xQueueSend(s_events, ev, 0) != pdTRUE) {
        s_dropped++;
    }
}

static void post_key(const io_key_event_t *k, void *ctx) {
    (void)ctx;
    io_event_t ev = {.type = IO_EV_KEY};
    ev.u.key = *k;
    post(&ev);
}

static void set_keys(const haltz_io_keys_t *next) {
    io_keys_diff(&s_in.keys, next, &s_in.caps_lock, post_key, NULL);
    s_in.keys = *next;
}

static void set_stick(const haltz_io_stick_t *st) {
    haltz_io_stick_t *old = &s_in.stick;
    if (st->x == old->x && st->y == old->y && st->buttons == old->buttons) {
        return;
    }
    io_event_t ev = {.type = IO_EV_STICK};
    ev.u.stick.now = *st;
    ev.u.stick.pressed = (uint8_t)(st->buttons & ~old->buttons);
    ev.u.stick.released = (uint8_t)(old->buttons & ~st->buttons);
    *old = *st;
    post(&ev);
}

static void set_knob(uint8_t v) {
    if (v == s_in.knob) {
        return;
    }
    s_in.knob = v;
    io_event_t ev = {.type = IO_EV_KNOB, .u.knob = v};
    post(&ev);
}

/* Link down: nothing stays pressed while the board is gone (protocol section 2). */
static void release_inputs(void) {
    haltz_io_keys_t none = {0};
    set_keys(&none);
    haltz_io_stick_t centre = {0};
    set_stick(&centre);
    s_in.devices = 0;
    s_in.keyboard = false;
}

/* ---- link state (task) ------------------------------------------------------------------ */

static void enter_state(io_link_state_t st) {
    if (st == s_state) {
        return;
    }
    bool was_up = s_state == IO_LINK_UP;
    s_state = st;
    if (was_up || st == IO_LINK_UP) {
        lock();
        if (was_up) {
            release_inputs();
        }
        io_event_t ev = {.type = IO_EV_LINK, .u.link_up = st == IO_LINK_UP};
        post(&ev);
        unlock();
    }
}

static void link_fail(const char *why) {
    s_reason = why;
    printf("io: %s, resetting the I/O board\n", why);
    enter_state(IO_LINK_RESET);
}

static void do_reset(void) {
    lock();
    s_resets++;
    s_have_status = false;
    unlock();
    gpio_set_level(PIN_IO_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(HALTZ_IO_RESET_MS));
    uart_flush_input(UART_NUM);
    xQueueReset(s_uart_q);
    haltz_decoder_init(&s_dec);
    gpio_set_level(PIN_IO_EN, 1); /* released: the board boots */
    s_tries++;
    s_hello_seen = false;
    s_hs_start_ms = now_ms();
    s_next_hello_ms = s_hs_start_ms;
    enter_state(IO_LINK_HANDSHAKE);
}

static void link_up(void) {
    s_up_since_ms = now_ms();
    s_last_rx_ms = s_up_since_ms;
    s_tries = 0;
    lock();
    s_link_ups++;
    send_kept_state();
    unlock();
    char git[9];
    haltz_git_hash_format(s_peer_git, git);
    printf("io: link up, I/O board protocol v%u git %s%s\n", s_peer_proto, git,
           s_peer_proto != HALTZ_PROTO_VERSION ? " - VERSION DIFFERS, re-flash a board" : "");
    enter_state(IO_LINK_UP);
}

static void board_rebooted(const char *why) {
    printf("io: the I/O board rebooted (%s), sending its state again\n", why);
    lock();
    s_reboots++;
    send_kept_state();
    unlock();
}

/* ---- receiving (task) ------------------------------------------------------------------ */

/* A warning, at most one per LOG_EVERY_MS (a bad wire must not flood the console). */
static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn(const char *fmt, ...) {
    int64_t now = now_ms();
    if (now - s_last_warn_ms < LOG_EVERY_MS) {
        return;
    }
    s_last_warn_ms = now;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* Any keyboard among the connected slots. */
static bool keyboard_connected(void) {
    for (int i = 0; i < HALTZ_IO_MAX_DEVICES; i++) {
        if ((s_in.devices & (1u << i)) && s_dev_kind[i] == HALTZ_IO_DEV_KEYBOARD) {
            return true;
        }
    }
    return false;
}

static void handle_rx(const haltz_frame_t *f) {
    s_last_rx_ms = now_ms();
    lock();
    s_rx_frames++;
    unlock();

    switch (f->type) {
    case HALTZ_MSG_HELLO: {
        haltz_hello_t h;
        if (haltz_hello_unpack(&h, f->payload, f->len) != HALTZ_OK || h.board_id != HALTZ_BOARD_IO) {
            break;
        }
        s_peer_proto = h.proto_version;
        memcpy(s_peer_git, h.git_hash, sizeof(s_peer_git));
        if (s_state == IO_LINK_HANDSHAKE) {
            s_hello_seen = true;
        } else if (s_state == IO_LINK_UP && now_ms() - s_up_since_ms > HELLO_GRACE_MS) {
            board_rebooted("unrequested HELLO");
        }
        return;
    }
    case HALTZ_MSG_NAK: {
        haltz_nak_t nak;
        if (haltz_nak_unpack(&nak, f->payload, f->len) == HALTZ_OK) {
            lock();
            s_naks++;
            unlock();
            warn("io: the board refused %s: error %u\n", haltz_msg_name(nak.type), nak.error);
        }
        return;
    }
    default:
        break;
    }

    haltz_err_t err = HALTZ_ERR_UNKNOWN_TYPE;
    bool rebooted = false;
    lock();
    switch (f->type) {
    case HALTZ_MSG_IO_KEYS: {
        haltz_io_keys_t k;
        err = haltz_io_keys_unpack(&k, f->payload, f->len);
        if (err == HALTZ_OK) {
            set_keys(&k);
        }
        break;
    }
    case HALTZ_MSG_IO_STICK: {
        haltz_io_stick_t st;
        err = haltz_io_stick_unpack(&st, f->payload, f->len);
        if (err == HALTZ_OK) {
            set_stick(&st);
        }
        break;
    }
    case HALTZ_MSG_IO_KNOB: {
        haltz_io_knob_t k;
        err = haltz_io_knob_unpack(&k, f->payload, f->len);
        if (err == HALTZ_OK) {
            set_knob(k.value);
        }
        break;
    }
    case HALTZ_MSG_IO_DEVICE: {
        haltz_io_device_t d;
        err = haltz_io_device_unpack(&d, f->payload, f->len);
        if (err == HALTZ_OK) {
            uint8_t bit = (uint8_t)(1u << d.slot);
            s_in.devices = d.state ? (uint8_t)(s_in.devices | bit) : (uint8_t)(s_in.devices & ~bit);
            s_dev_kind[d.slot] = d.kind;
            s_in.keyboard = keyboard_connected();
            io_event_t ev = {.type = IO_EV_DEVICE, .u.device = d};
            post(&ev);
        }
        break;
    }
    case HALTZ_MSG_IO_STATUS: {
        haltz_io_status_t st;
        err = haltz_io_status_unpack(&st, f->payload, f->len);
        if (err == HALTZ_OK) {
            rebooted = s_have_status && st.uptime_ms < s_status.uptime_ms;
            s_status = st;
            s_have_status = true;
            s_status_ms = now_ms();
            s_in.devices = st.devices;
            s_in.keyboard = (st.flags & HALTZ_IO_STATUS_KEYBOARD) != 0;
            s_in.stick_ok = (st.flags & HALTZ_IO_STATUS_STICK_OK) != 0;
        }
        break;
    }
    default:
        break;
    }
    if (err != HALTZ_OK) {
        s_rx_bad++;
    }
    unlock();
    if (err != HALTZ_OK) {
        warn("io: dropped a bad %s from the board: error %d\n", haltz_msg_name(f->type), (int)err);
    }
    if (rebooted && s_state == IO_LINK_UP) {
        board_rebooted("uptime went back");
    }
}

static void decoder_results(haltz_dec_result_t r) {
    for (; r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&s_dec)) {
        if (r == HALTZ_DEC_FRAME) {
            handle_rx(haltz_decoder_frame(&s_dec));
        } else if (r == HALTZ_DEC_CRC_ERROR) {
            lock();
            s_rx_crc++;
            unlock();
        }
    }
}

/* ---- the task ---------------------------------------------------------------------------- */

static void io_task(void *arg) {
    (void)arg;
    static uint8_t buf[256];
    int64_t last_byte_ms = 0;
    bool flushed = true;
    for (;;) {
        if (s_reset_req) {
            s_reset_req = false;
            s_reason = "requested";
            s_tries = 0;
            enter_state(IO_LINK_RESET);
        }
        if (s_state == IO_LINK_RESET) {
            do_reset();
            continue;
        }
        uart_event_t ev;
        if (xQueueReceive(s_uart_q, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE &&
            (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL)) {
            uart_flush_input(UART_NUM);
            xQueueReset(s_uart_q);
            decoder_results(haltz_decoder_flush(&s_dec));
            warn("io: UART1 receive buffer overflowed, data lost\n");
        }
        size_t avail = 0;
        uart_get_buffered_data_len(UART_NUM, &avail);
        while (avail > 0) {
            int n = uart_read_bytes(UART_NUM, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n; i++) {
                decoder_results(haltz_decoder_feed(&s_dec, buf[i]));
            }
            last_byte_ms = now_ms();
            flushed = false;
            avail = avail > (size_t)n ? avail - (size_t)n : 0;
        }

        int64_t now = now_ms();
        if (!flushed && now - last_byte_ms >= HALTZ_IO_IDLE_FLUSH_MS) {
            decoder_results(haltz_decoder_flush(&s_dec));
            flushed = true;
        }

        switch (s_state) {
        case IO_LINK_HANDSHAKE:
            if (s_hello_seen) {
                link_up();
            } else if (now - s_hs_start_ms > HALTZ_IO_HELLO_TIMEOUT_MS) {
                if (s_tries >= HALTZ_IO_HELLO_TRIES) {
                    printf("io: no answer from the I/O board after %d resets; trying again "
                           "every %d s\n",
                           s_tries, MISSING_RETRY_MS / 1000);
                    s_reason = "no HELLO from the board";
                    s_retry_ms = now + MISSING_RETRY_MS;
                    enter_state(IO_LINK_MISSING);
                } else {
                    s_reason = "no HELLO within 3 s";
                    enter_state(IO_LINK_RESET);
                }
            } else if (now >= s_next_hello_ms) {
                s_next_hello_ms = now + HALTZ_IO_HELLO_EVERY_MS;
                lock();
                send_hello();
                unlock();
            }
            break;
        case IO_LINK_UP:
            if (now - s_last_rx_ms > HALTZ_IO_DEAD_MS) {
                link_fail("nothing from the I/O board for 1.5 s");
            }
            break;
        case IO_LINK_MISSING:
            if (now >= s_retry_ms) {
                s_tries = 0;
                enter_state(IO_LINK_RESET);
            }
            break;
        default:
            break;
        }
    }
}

esp_err_t io_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(io_event_t));
    if (s_lock == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* EN: open drain, released (high via the DevKit's pull-up) until the first reset. */
    gpio_config_t en = {.pin_bit_mask = 1ULL << PIN_IO_EN, .mode = GPIO_MODE_OUTPUT_OD};
    esp_err_t err = gpio_config(&en);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_IO_EN, 1);

    uart_config_t cfg = {.baud_rate = HALTZ_IO_BAUD,
                         .data_bits = UART_DATA_8_BITS,
                         .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                         .source_clk = UART_SCLK_DEFAULT};
    err = uart_driver_install(UART_NUM, UART_RX_BUF, UART_TX_BUF, UART_QUEUE_LEN, &s_uart_q, 0);
    if (err == ESP_OK) {
        err = uart_param_config(UART_NUM, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(UART_NUM, PIN_IO_UART_TX, PIN_IO_UART_RX, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        return err;
    }
    gpio_pullup_en(PIN_IO_UART_RX); /* open RX idles high instead of picking up noise */
    haltz_decoder_init(&s_dec);
    s_reason = "boot";

    if (xTaskCreatePinnedToCore(io_task, "io", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- public ------------------------------------------------------------------------------ */

bool io_is_up(void) {
    return s_state == IO_LINK_UP;
}

bool io_wait_up(uint32_t timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    while (!io_is_up()) {
        if (now_ms() >= end) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

const char *io_link_state_name(io_link_state_t s) {
    switch (s) {
    case IO_LINK_RESET:
        return "RESET";
    case IO_LINK_HANDSHAKE:
        return "HANDSHAKE";
    case IO_LINK_UP:
        return "UP";
    case IO_LINK_MISSING:
        return "MISSING";
    default:
        return "?";
    }
}

void io_get_stats(io_stats_t *out) {
    memset(out, 0, sizeof(*out));
    lock();
    out->state = s_state;
    out->peer_proto = s_peer_proto;
    haltz_git_hash_format(s_peer_git, out->peer_git);
    out->resets = s_resets;
    out->link_ups = s_link_ups;
    out->reboots = s_reboots;
    out->last_reset_reason = s_reason;
    out->rx_frames = s_rx_frames;
    out->rx_crc_errors = s_rx_crc;
    out->rx_bad = s_rx_bad;
    out->naks = s_naks;
    out->tx_frames = s_tx_frames;
    out->tx_bytes = s_tx_bytes;
    out->events_dropped = s_dropped;
    out->have_status = s_have_status;
    out->status = s_status;
    out->status_age_ms = s_have_status ? (uint32_t)(now_ms() - s_status_ms) : 0;
    unlock();
}

void io_force_reset(void) {
    s_reset_req = true; /* the task does it: it owns the link state */
}

bool io_get_event(io_event_t *ev, uint32_t timeout_ms) {
    return xQueueReceive(s_events, ev, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void io_get_input(io_input_t *out) {
    lock();
    *out = s_in;
    unlock();
}

static bool is_sound(uint8_t type) {
    return type == HALTZ_MSG_IO_TONE || type == HALTZ_MSG_IO_NOTES || type == HALTZ_MSG_IO_SFX ||
           type == HALTZ_MSG_IO_STOP;
}

haltz_err_t io_cmd(const haltz_io_cmd_t *cmd) {
    if (cmd == NULL) {
        return HALTZ_ERR_ARG;
    }
    uint8_t p[HALTZ_MAX_PAYLOAD];
    size_t n = 0;
    haltz_err_t err = haltz_io_cmd_pack(cmd, p, sizeof(p), &n);
    if (err != HALTZ_OK) {
        return err;
    }
    lock();
    switch (cmd->type) { /* the kept state, sent again at link up */
    case HALTZ_MSG_IO_LED:
        if (cmd->u.led.mode != HALTZ_IO_LED_FLASH) {
            s_keep_led = cmd->u.led;
            s_keep_led_valid = true;
        }
        break;
    case HALTZ_MSG_IO_VOLUME:
        for (int v = 0; v < HALTZ_IO_VOICES; v++) {
            if (cmd->u.volume.voices & (1u << v)) {
                s_keep_volume[v] = cmd->u.volume.level;
            }
        }
        break;
    case HALTZ_MSG_IO_BT:
        if (cmd->u.bt.action != HALTZ_IO_BT_FORGET) {
            s_keep_scan = cmd->u.bt.action == HALTZ_IO_BT_SCAN_ON;
        }
        break;
    default:
        break;
    }
    if (s_state != IO_LINK_UP) {
        unlock();
        return is_sound(cmd->type) ? HALTZ_ERR_BUSY : HALTZ_OK;
    }
    send_frame(cmd->type, p, n);
    unlock();
    return HALTZ_OK;
}

haltz_err_t io_led(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms,
                   uint8_t count) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_LED};
    c.u.led = (haltz_io_led_t){mode, r, g, b, period_ms, count};
    return io_cmd(&c);
}

haltz_err_t io_tone(uint8_t voice, uint16_t hz, uint16_t ms) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_TONE};
    c.u.tone = (haltz_io_tone_t){.voice = voice, .hz = hz, .ms = ms};
    return io_cmd(&c);
}

haltz_err_t io_notes(uint8_t voice, uint8_t flags, uint8_t tick_ms, const haltz_io_note_t *notes,
                     size_t n) {
    if (notes == NULL || n == 0) {
        return HALTZ_ERR_ARG;
    }
    if (n > HALTZ_IO_TUNE_NOTES) {
        return HALTZ_ERR_NO_SPACE;
    }
    static haltz_io_cmd_t c; /* ~260 bytes; under s_notes_lock */
    static SemaphoreHandle_t s_notes_lock;
    if (s_notes_lock == NULL) {
        lock();
        if (s_notes_lock == NULL) {
            s_notes_lock = xSemaphoreCreateMutex();
        }
        unlock();
    }
    xSemaphoreTake(s_notes_lock, portMAX_DELAY);
    haltz_err_t err = HALTZ_OK;
    for (int pass = 0; pass < 2 && err == HALTZ_OK; pass++) { /* 0: check all, 1: send */
        for (size_t first = 0; first < n && err == HALTZ_OK; first += HALTZ_IO_NOTES_MAX) {
            size_t cnt = n - first < HALTZ_IO_NOTES_MAX ? n - first : HALTZ_IO_NOTES_MAX;
            c.type = HALTZ_MSG_IO_NOTES;
            c.u.notes.voice = voice;
            c.u.notes.flags = first == 0 ? flags : (uint8_t)(flags & ~HALTZ_IO_NOTES_REPLACE);
            c.u.notes.tick_ms = tick_ms;
            c.u.notes.count = (uint8_t)cnt;
            memcpy(c.u.notes.notes, &notes[first], cnt * sizeof(notes[0]));
            if (pass == 0) {
                uint8_t p[HALTZ_MAX_PAYLOAD];
                err = haltz_io_cmd_pack(&c, p, sizeof(p), NULL);
            } else {
                err = io_cmd(&c);
            }
        }
    }
    xSemaphoreGive(s_notes_lock);
    return err;
}

haltz_err_t io_sfx(uint8_t id, uint8_t voice) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_SFX};
    c.u.sfx = (haltz_io_sfx_msg_t){.id = id, .voice = voice};
    return io_cmd(&c);
}

haltz_err_t io_stop(uint8_t voices) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_STOP};
    c.u.stop.voices = voices;
    return io_cmd(&c);
}

haltz_err_t io_volume(uint8_t voices, uint8_t level) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_VOLUME};
    c.u.volume = (haltz_io_volume_t){.voices = voices, .level = level};
    return io_cmd(&c);
}

haltz_err_t io_stick_calibrate(void) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_STICK_CAL};
    return io_cmd(&c);
}

haltz_err_t io_bt(uint8_t action) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_BT};
    c.u.bt.action = action;
    return io_cmd(&c);
}
