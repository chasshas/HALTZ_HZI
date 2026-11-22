/*
 * HALTZ I/O: the UART link to the S3. See io_link.h.
 *
 * One task (core 1) owns UART2 in both directions:
 *   - RX: UART driver events -> decoder; a frame left half done for 10 ms is flushed.
 *     HELLO is answered with HELLO, commands are unpacked and handed to io_fx.c / io_input.c /
 *     the Bluetooth platform; a refused command is answered with NAK.
 *   - every 10 ms: one stick/knob/button sample; IO_STICK / IO_KNOB when they changed.
 *   - the Bluetooth queue: keyboard reports -> merged keyboard state -> IO_KEYS when it
 *     changed; connects / disconnects / battery -> IO_DEVICE.
 *   - every 250 ms: IO_STATUS and the refresh (IO_KEYS, IO_STICK, IO_KNOB again).
 * Only this task writes to the UART, so frames never interleave.
 */
#include "io_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "haltz/board_ids.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"
#include "haltz_platform.h"
#include "io_fx.h"
#include "io_input.h"

#if __has_include("haltz_git_hash.h")
#include "haltz_git_hash.h"
#endif
#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

#define UART_NUM       UART_NUM_2
#define UART_RX_BUF    2048
#define UART_TX_BUF    2048
#define UART_QUEUE_LEN 16
#define SAMPLE_MS      10 /* stick / knob / buttons, 100 Hz */
#define TASK_STACK     6144
#define TASK_PRIO      6
#define TASK_CORE      1

static QueueHandle_t s_uart_q;
static haltz_decoder_t s_dec;
static volatile bool s_watch;

/* Link task state (read by io_link_get_stats() without a lock: it's for the console). */
static uint32_t s_rx_frames, s_crc_errors, s_cmd_errors, s_rx_overruns;
static uint32_t s_tx_frames, s_tx_bytes, s_naks_sent;
static uint32_t s_s3_hellos;
static uint16_t s_s3_proto;
static uint8_t s_s3_git[4];
static int64_t s_last_good_rx_ms = -1;

/* Keyboard state per Bluetooth slot, and the merged state that IO_KEYS carries. */
static haltz_io_keys_t s_slot_keys[HALTZ_IO_MAX_DEVICES];
static haltz_io_keys_t s_keys;
static uint8_t s_dev_kind[HALTZ_IO_MAX_DEVICES];
static uint8_t s_dev_battery[HALTZ_IO_MAX_DEVICES];
static uint8_t s_devices; /* bit n = slot n connected */

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* ---- sending ----------------------------------------------------------------------------- */

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

static void send_hello(void) {
    haltz_hello_t h = {.board_id = HALTZ_BOARD_IO, .proto_version = HALTZ_PROTO_VERSION};
    haltz_git_hash_parse(HALTZ_GIT_HASH, h.git_hash);
    uint8_t p[HALTZ_MSG_HELLO_LEN];
    if (haltz_hello_pack(&h, p, sizeof(p)) == HALTZ_OK) {
        send_frame(HALTZ_MSG_HELLO, p, sizeof(p));
    }
}

static void send_nak(uint8_t type, haltz_err_t err) {
    s_cmd_errors++;
    haltz_nak_t nak = {.type = type, .error = (uint8_t)err};
    uint8_t p[HALTZ_MSG_NAK_LEN];
    if (haltz_nak_pack(&nak, p, sizeof(p)) == HALTZ_OK) {
        send_frame(HALTZ_MSG_NAK, p, sizeof(p));
        s_naks_sent++;
    }
    if (s_watch) {
        printf("io: NAK %s: error %d\n", haltz_msg_name(type), (int)err);
    }
}

static void send_keys(bool changed) {
    uint8_t p[HALTZ_MSG_IO_KEYS_MAX_LEN];
    size_t n = 0;
    if (haltz_io_keys_pack(&s_keys, p, sizeof(p), &n) == HALTZ_OK) {
        send_frame(HALTZ_MSG_IO_KEYS, p, n);
    }
    if (changed && s_watch) {
        printf("io: KEYS mods %02X:", s_keys.modifiers);
        for (int i = 0; i < s_keys.count; i++) {
            printf(" %02X", s_keys.usages[i]);
        }
        printf("\n");
    }
}

static void send_stick(bool changed) {
    const haltz_io_stick_t *st = &input_state()->stick;
    uint8_t p[HALTZ_MSG_IO_STICK_LEN];
    size_t n = 0;
    if (haltz_io_stick_pack(st, p, sizeof(p), &n) == HALTZ_OK) {
        send_frame(HALTZ_MSG_IO_STICK, p, n);
    }
    if (changed && s_watch) {
        printf("io: STICK x %+5d y %+5d buttons %u\n", st->x, st->y, st->buttons);
    }
}

static void send_knob(bool changed) {
    haltz_io_knob_t k = {.value = input_state()->knob};
    uint8_t p[HALTZ_MSG_IO_KNOB_LEN];
    size_t n = 0;
    if (haltz_io_knob_pack(&k, p, sizeof(p), &n) == HALTZ_OK) {
        send_frame(HALTZ_MSG_IO_KNOB, p, n);
    }
    if (changed && s_watch) {
        printf("io: KNOB %u\n", k.value);
    }
}

static void send_device(const bt_event_t *ev, bool connected) {
    haltz_io_device_t d = {.slot = ev->slot,
                           .state = connected ? HALTZ_IO_DEV_CONNECTED : HALTZ_IO_DEV_DISCONNECTED,
                           .kind = ev->kind,
                           .battery = ev->battery,
                           .vendor_id = ev->vendor_id,
                           .product_id = ev->product_id};
    uint8_t p[HALTZ_MSG_IO_DEVICE_LEN];
    size_t n = 0;
    if (haltz_io_device_pack(&d, p, sizeof(p), &n) == HALTZ_OK) {
        send_frame(HALTZ_MSG_IO_DEVICE, p, n);
    }
    if (s_watch) {
        printf("io: DEVICE slot %u %s kind %u battery %u\n", d.slot,
               connected ? "connected" : "disconnected", d.kind, d.battery);
    }
}

static void build_status(haltz_io_status_t *st) {
    memset(st, 0, sizeof(*st));
    st->uptime_ms = (uint32_t)now_ms();
    st->rx_frames = (uint16_t)s_rx_frames;
    st->crc_errors = (uint16_t)s_crc_errors;
    st->cmd_errors = (uint16_t)s_cmd_errors;
    st->rx_overruns = (uint16_t)s_rx_overruns;
    st->flags = fx_status(st->tune_free);
    for (int i = 0; i < HALTZ_IO_MAX_DEVICES; i++) {
        if ((s_devices & (1u << i)) && s_dev_kind[i] == HALTZ_IO_DEV_KEYBOARD) {
            st->flags |= HALTZ_IO_STATUS_KEYBOARD;
        }
    }
    if (haltz_bt_scanning()) {
        st->flags |= HALTZ_IO_STATUS_SCANNING;
    }
    if (input_state()->stick_ok) {
        st->flags |= HALTZ_IO_STATUS_STICK_OK;
    }
    st->devices = s_devices;
}

static void send_status_and_refresh(void) {
    haltz_io_status_t st;
    build_status(&st);
    uint8_t p[HALTZ_MSG_IO_STATUS_LEN];
    size_t n = 0;
    if (haltz_io_status_pack(&st, p, sizeof(p), &n) == HALTZ_OK) {
        send_frame(HALTZ_MSG_IO_STATUS, p, n);
    }
    send_keys(false);
    send_stick(false);
    send_knob(false);
}

/* ---- keyboard state ---------------------------------------------------------------------- */

static bool keys_equal(const haltz_io_keys_t *a, const haltz_io_keys_t *b) {
    return a->modifiers == b->modifiers && a->count == b->count &&
           memcmp(a->usages, b->usages, a->count) == 0;
}

/* All slots ORed together, at most HALTZ_IO_KEYS_MAX keys, in the order they come. */
static void merge_keys(haltz_io_keys_t *out) {
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < HALTZ_IO_MAX_DEVICES; s++) {
        out->modifiers |= s_slot_keys[s].modifiers;
        for (int i = 0; i < s_slot_keys[s].count && out->count < HALTZ_IO_KEYS_MAX; i++) {
            uint8_t u = s_slot_keys[s].usages[i];
            bool dup = false;
            for (int j = 0; j < out->count; j++) {
                dup |= out->usages[j] == u;
            }
            if (!dup) {
                out->usages[out->count++] = u;
            }
        }
    }
}

/* A report from one keyboard. Rollover-error reports are skipped (protocol section 3). */
static void slot_report(int slot, uint8_t modifiers, const uint8_t keys[HALTZ_IO_KEYS_MAX]) {
    haltz_io_keys_t k = {.modifiers = modifiers};
    for (int i = 0; i < HALTZ_IO_KEYS_MAX; i++) {
        uint8_t u = keys[i];
        if (u >= 0x01 && u <= 0x03) {
            return; /* "too many keys": keep the last good state */
        }
        if (u < HALTZ_IO_USAGE_FIRST || u > HALTZ_IO_USAGE_LAST) {
            continue; /* empty entry (0) or a modifier usage */
        }
        bool dup = false;
        for (int j = 0; j < k.count; j++) {
            dup |= k.usages[j] == u;
        }
        if (!dup) {
            k.usages[k.count++] = u;
        }
    }
    s_slot_keys[slot] = k;
}

static void update_keys(void) {
    haltz_io_keys_t merged;
    merge_keys(&merged);
    if (!keys_equal(&merged, &s_keys)) {
        s_keys = merged;
        send_keys(true);
    }
}

static void drain_bluetooth(void) {
    bt_event_t ev;
    bool keys_touched = false;
    while (haltz_bt_next(&ev)) {
        if (ev.slot >= HALTZ_IO_MAX_DEVICES) {
            continue;
        }
        switch (ev.type) {
        case BT_EV_KEYS:
            slot_report(ev.slot, ev.modifiers, ev.keys);
            /* Each report is sent on its own, so a quick tap (down, up) shows as two
             * IO_KEYS even when both reports came in the same drain. */
            update_keys();
            break;
        case BT_EV_CONNECT:
            s_devices |= (uint8_t)(1u << ev.slot);
            s_dev_kind[ev.slot] = ev.kind;
            s_dev_battery[ev.slot] = ev.battery;
            memset(&s_slot_keys[ev.slot], 0, sizeof(s_slot_keys[0]));
            send_device(&ev, true);
            break;
        case BT_EV_DISCONNECT:
            s_devices &= (uint8_t) ~(1u << ev.slot);
            memset(&s_slot_keys[ev.slot], 0, sizeof(s_slot_keys[0]));
            keys_touched = true;
            send_device(&ev, false);
            break;
        case BT_EV_BATTERY:
            s_dev_battery[ev.slot] = ev.battery;
            if (s_devices & (1u << ev.slot)) {
                send_device(&ev, true);
            }
            break;
        default:
            break;
        }
    }
    if (keys_touched) {
        update_keys(); /* keys of a keyboard that went away are released */
    }
}

/* ---- receiving --------------------------------------------------------------------------- */

static haltz_io_cmd_t s_cmd; /* ~260 bytes: off the stack */

static void handle_frame(const haltz_frame_t *f) {
    s_rx_frames++;
    s_last_good_rx_ms = now_ms();
    if (f->type == HALTZ_MSG_HELLO) {
        haltz_hello_t h;
        if (haltz_hello_unpack(&h, f->payload, f->len) != HALTZ_OK) {
            send_nak(f->type, HALTZ_ERR_BAD_LEN);
            return;
        }
        if (s_s3_hellos == 0 || h.proto_version != s_s3_proto) {
            char git[9];
            haltz_git_hash_format(h.git_hash, git);
            printf("io: HELLO from board %u, protocol v%u, git %s%s\n", h.board_id,
                   h.proto_version, git,
                   h.proto_version != HALTZ_PROTO_VERSION ? " (DIFFERENT VERSION)" : "");
        }
        s_s3_hellos++;
        s_s3_proto = h.proto_version;
        memcpy(s_s3_git, h.git_hash, sizeof(s_s3_git));
        send_hello();
        return;
    }

    haltz_err_t err = haltz_io_cmd_unpack(&s_cmd, f->type, f->payload, f->len);
    if (err != HALTZ_OK) {
        send_nak(f->type, err);
        return;
    }
    if (s_watch) {
        printf("io: got %s (%u bytes)\n", haltz_msg_name(f->type), f->len);
    }
    switch (s_cmd.type) {
    case HALTZ_MSG_IO_STICK_CAL:
        input_calibrate();
        printf("io: stick centre %d %d %s\n", input_state()->centre_x, input_state()->centre_y,
               input_state()->stick_ok ? "OK" : "IMPLAUSIBLE (wiring?)");
        break;
    case HALTZ_MSG_IO_BT:
        haltz_bt_action(s_cmd.u.bt.action);
        break;
    default:
        err = fx_cmd(&s_cmd);
        if (err != HALTZ_OK) {
            send_nak(f->type, err);
        }
        break;
    }
}

static void feed(const uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) {
        haltz_dec_result_t r = haltz_decoder_feed(&s_dec, buf[i]);
        for (; r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&s_dec)) {
            if (r == HALTZ_DEC_FRAME) {
                handle_frame(haltz_decoder_frame(&s_dec));
            } else if (r == HALTZ_DEC_CRC_ERROR) {
                s_crc_errors++;
            }
        }
    }
}

static void flush_decoder(void) {
    for (haltz_dec_result_t r = haltz_decoder_flush(&s_dec); r != HALTZ_DEC_NEED_MORE;
         r = haltz_decoder_poll(&s_dec)) {
        if (r == HALTZ_DEC_FRAME) {
            handle_frame(haltz_decoder_frame(&s_dec));
        } else if (r == HALTZ_DEC_CRC_ERROR) {
            s_crc_errors++;
        }
    }
}

/* ---- the task ---------------------------------------------------------------------------- */

static void link_task(void *arg) {
    (void)arg;
    static uint8_t buf[256];
    int64_t next_sample = now_ms();
    int64_t next_status = next_sample + HALTZ_IO_STATUS_EVERY_MS;
    int64_t last_rx_ms = 0;
    bool flushed = true;

    send_hello();
    for (;;) {
        int64_t now = now_ms();
        int64_t until = next_sample < next_status ? next_sample : next_status;
        int64_t wait = until - now;
        if (!flushed) {
            int64_t f = last_rx_ms + HALTZ_IO_IDLE_FLUSH_MS - now;
            wait = f < wait ? f : wait;
        }
        wait = wait < 1 ? 1 : (wait > SAMPLE_MS ? SAMPLE_MS : wait);

        uart_event_t ev;
        if (xQueueReceive(s_uart_q, &ev, pdMS_TO_TICKS(wait)) == pdTRUE) {
            if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                s_rx_overruns++;
                uart_flush_input(UART_NUM);
                xQueueReset(s_uart_q);
                flush_decoder();
            }
        }
        size_t avail = 0;
        uart_get_buffered_data_len(UART_NUM, &avail);
        while (avail > 0) {
            int n = uart_read_bytes(UART_NUM, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            feed(buf, n);
            last_rx_ms = now_ms();
            flushed = false;
            avail = avail > (size_t)n ? avail - (size_t)n : 0;
        }

        now = now_ms();
        if (!flushed && now - last_rx_ms >= HALTZ_IO_IDLE_FLUSH_MS) {
            flush_decoder();
            flushed = true;
        }
        drain_bluetooth();
        if (now >= next_sample) {
            next_sample += SAMPLE_MS;
            if (next_sample <= now) {
                next_sample = now + SAMPLE_MS; /* fell behind: don't catch up */
            }
            bool stick_changed, knob_changed;
            input_sample(&stick_changed, &knob_changed);
            if (stick_changed) {
                send_stick(true);
            }
            if (knob_changed) {
                send_knob(true);
            }
        }
        if (now >= next_status) {
            next_status += HALTZ_IO_STATUS_EVERY_MS;
            if (next_status <= now) {
                next_status = now + HALTZ_IO_STATUS_EVERY_MS;
            }
            send_status_and_refresh();
        }
    }
}

esp_err_t io_start(void) {
    esp_err_t err = fx_start();
    if (err != ESP_OK) {
        printf("io: sound/LED did not start: %s\n", esp_err_to_name(err));
        return err;
    }
    err = input_start();
    if (err != ESP_OK) {
        printf("io: stick/knob did not start: %s\n", esp_err_to_name(err));
        return err;
    }
    const input_state_t *in = input_state();
    printf("io: stick centre %d %d %s, knob %u\n", in->centre_x, in->centre_y,
           in->stick_ok ? "OK" : "IMPLAUSIBLE (wiring? the axes will read 0)", in->knob);

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
        err = uart_set_pin(UART_NUM, PIN_CPU_UART_TX, PIN_CPU_UART_RX, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        printf("io: UART2 did not start: %s\n", esp_err_to_name(err));
        return err;
    }
    gpio_pullup_en(PIN_CPU_UART_RX); /* open RX idles high instead of picking up noise */
    haltz_decoder_init(&s_dec);

    if (xTaskCreatePinnedToCore(link_task, "io_link", TASK_STACK, NULL, TASK_PRIO, NULL,
                                TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    printf("io: link on UART2 at %d baud (TX GPIO%d, RX GPIO%d)\n", HALTZ_IO_BAUD,
           PIN_CPU_UART_TX, PIN_CPU_UART_RX);
    return ESP_OK;
}

/* ---- console ----------------------------------------------------------------------------- */

void io_link_get_stats(io_link_stats_t *out) {
    memset(out, 0, sizeof(*out));
    int64_t now = now_ms();
    out->uptime_ms = (uint32_t)now;
    out->s3_hellos = s_s3_hellos;
    out->s3_proto = s_s3_proto;
    haltz_git_hash_format(s_s3_git, out->s3_git);
    out->last_rx_age_ms = s_last_good_rx_ms < 0 ? UINT32_MAX : (uint32_t)(now - s_last_good_rx_ms);
    build_status(&out->status);
    out->tx_frames = s_tx_frames;
    out->tx_bytes = s_tx_bytes;
    out->naks_sent = s_naks_sent;
    out->bt_dropped = haltz_bt_dropped();
    out->keys = s_keys;
    memcpy(out->device_kind, s_dev_kind, sizeof(out->device_kind));
    memcpy(out->device_battery, s_dev_battery, sizeof(out->device_battery));
}

void io_link_set_watch(bool on) {
    s_watch = on;
}

bool io_link_watch(void) {
    return s_watch;
}
