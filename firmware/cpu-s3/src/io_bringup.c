/*
 * HALTZ CPU: I/O link part of the bring-up screen. See io_bringup.h.
 *
 * Screen: two status rows (link state and versions; keyboard, stick, buttons, knob) and the
 * pointer, sprite 63 above the text (gpu-protocol-v1 convention), moved by the stick.
 * Typed keys appear in the console window. LED: blinks blue while no keyboard is connected,
 * green when one is. BOOT chime at link up; the stick click plays CLICK.
 * Keys on the USB console:
 *   i  link statistics
 *   p  two-voice test tune
 *   b  next sound effect
 *   l  next LED pattern
 *   c  measure the stick centre again (leave the stick alone)
 *   e  reset the I/O board (the keyboard reconnects after a few seconds)
 *   k  link test: 2000 LED commands as fast as the UART takes them; PASS if the board
 *      counted exactly those frames and nobody saw a CRC error, NAK or overrun
 */
#include "io_bringup.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpu.h"
#include "io.h"

#define POINTER_SPRITE  HALTZ_GPU_SPRITE_CURSOR
#define POINTER_PATTERN 255
#define POINTER_BANK    14  /* bank 14: p=7 is index 231 (white), p=8 index 232 (near black) */
#define POINTER_SPEED   420 /* pixels per second at full deflection */
#define LINK_TEST_CMDS  2000

static int s_row;
static float s_px = HALTZ_GPU_SCREEN_W / 2, s_py = HALTZ_GPU_SCREEN_H / 2;
static int s_drawn_x = -1000, s_drawn_y = -1000;
static int64_t s_last_move_us;
static bool s_was_up, s_had_keyboard;
static int s_sfx_next, s_led_next;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* ---- pointer ----------------------------------------------------------------------------- */

static const char *const k_arrow[16] = {
    "8...............", "88..............", "878.............", "8778............",
    "87778...........", "877778..........", "8777778.........", "87777778........",
    "877777778.......", "8777777778......", "87777788888.....", "8778778.........",
    "878.8778........", "88...8778.......", "8.....8778......", "......888.......",
};

static void load_pointer(void) {
    uint8_t d[HALTZ_GPU_PATTERN_BYTES] = {0};
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            char c = k_arrow[y][x];
            uint8_t p = (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : 0;
            d[y * 8 + x / 2] |= (x % 2 == 0) ? (uint8_t)(p << 4) : p; /* high nibble = left */
        }
    }
    gpu_pattern_load(POINTER_PATTERN, d);
    s_drawn_x = s_drawn_y = -1000; /* the sprite is set again by the next poll */
}

static bool move_pointer(const haltz_io_stick_t *st) {
    int64_t now = esp_timer_get_time();
    float dt = s_last_move_us ? (float)(now - s_last_move_us) / 1e6f : 0.0f;
    s_last_move_us = now;
    if (dt > 0.1f) {
        dt = 0.1f; /* after a pause (demo, stress test): no jump */
    }
    /* Speed grows with the square of the deflection: fine control near the centre. */
    float fx = st->x / (float)HALTZ_IO_AXIS_MAX, fy = st->y / (float)HALTZ_IO_AXIS_MAX;
    s_px += fx * fabsf(fx) * POINTER_SPEED * dt;
    s_py -= fy * fabsf(fy) * POINTER_SPEED * dt; /* stick up = +y, screen y goes down */
    s_px = s_px < 0 ? 0 : (s_px > HALTZ_GPU_SCREEN_W - 1 ? HALTZ_GPU_SCREEN_W - 1 : s_px);
    s_py = s_py < 0 ? 0 : (s_py > HALTZ_GPU_SCREEN_H - 1 ? HALTZ_GPU_SCREEN_H - 1 : s_py);
    int x = (int)s_px, y = (int)s_py;
    if (x == s_drawn_x && y == s_drawn_y) {
        return false;
    }
    s_drawn_x = x;
    s_drawn_y = y;
    gpu_sprite_set(POINTER_SPRITE, (int16_t)x, (int16_t)y, POINTER_PATTERN,
                   (uint8_t)(POINTER_BANK | HALTZ_GPU_SPR_ABOVE_TEXT |
                             HALTZ_GPU_SPR_VISIBLE));
    return true;
}

/* ---- status rows ------------------------------------------------------------------------- */

static void put_row(int row, uint8_t fg, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void put_row(int row, uint8_t fg, const char *fmt, ...) {
    char buf[HALTZ_GPU_TEXT_COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n < HALTZ_GPU_TEXT_COLS) {
        buf[n++] = ' ';
    }
    gpu_text_put(0, (uint8_t)row, fg, 0, (const uint8_t *)buf, HALTZ_GPU_TEXT_COLS);
}

void io_bringup_draw(void) {
    io_stats_t st;
    io_get_stats(&st);
    io_input_t in;
    io_get_input(&in);
    bool up = st.state == IO_LINK_UP;
    bool mismatch = up && st.peer_proto != HALTZ_PROTO_VERSION;
    const haltz_io_status_t *s = &st.status;
    bool bad = s->crc_errors || s->cmd_errors || s->rx_overruns || st.rx_crc_errors ||
               st.rx_bad || st.naks;
    put_row(s_row, (!up || mismatch) ? HALTZ_COLOR_RED : (bad ? HALTZ_COLOR_YELLOW : HALTZ_COLOR_WHITE),
            " IO v%u %s %s%s %s", (unsigned)st.peer_proto, st.peer_git,
            io_link_state_name(st.state), mismatch ? " DIFFERS" : "",
            !up ? "" : (in.keyboard ? "kbd" : "no kbd"));
    if (!up) {
        put_row(s_row + 1, HALTZ_COLOR_LIGHT_GREY, " resets %" PRIu32 " (%s)", st.resets,
                st.last_reset_reason ? st.last_reset_reason : "-");
        return;
    }
    put_row(s_row + 1, in.stick_ok ? HALTZ_COLOR_LIGHT_GREY : HALTZ_COLOR_RED,
            " x%+5d y%+5d b%u knob %3u %s%s", in.stick.x, in.stick.y, in.stick.buttons, in.knob,
            in.caps_lock ? "CAPS " : "", in.stick_ok ? "" : "STICK?");
}

/* ---- LED policy and sounds --------------------------------------------------------------- */

static void led_policy(bool keyboard) {
    if (keyboard) {
        io_led(HALTZ_IO_LED_SOLID, 0, 160, 0, 0, 0); /* green: ready */
    } else {
        io_led(HALTZ_IO_LED_BLINK, 0, 0, 255, 800, 0); /* blue blink: waiting for the keyboard */
    }
}

static void play_test_tune(void) {
    /* Two voices, tick 125 ms: arpeggios on A, roots on B. */
    static const uint8_t melody[] = {72, 2, 76, 2, 79, 2, 84, 2, 83, 2, 79, 2, 74, 2, 79, 2,
                                     81, 2, 77, 2, 72, 2, 77, 2, 79, 2, 76, 2, 72, 4};
    static const uint8_t bass[] = {48, 8, 43, 8, 41, 8, 48, 6};
    haltz_io_note_t a[sizeof(melody) / 2], b[sizeof(bass) / 2];
    for (size_t i = 0; i < sizeof(melody) / 2; i++) {
        a[i] = (haltz_io_note_t){melody[2 * i], melody[2 * i + 1]};
    }
    for (size_t i = 0; i < sizeof(bass) / 2; i++) {
        b[i] = (haltz_io_note_t){bass[2 * i], bass[2 * i + 1]};
    }
    haltz_err_t e1 = io_notes(HALTZ_IO_VOICE_A, HALTZ_IO_NOTES_REPLACE, 125, a, sizeof(a) / sizeof(a[0]));
    haltz_err_t e2 = io_notes(HALTZ_IO_VOICE_B, HALTZ_IO_NOTES_REPLACE, 125, b, sizeof(b) / sizeof(b[0]));
    printf("io: test tune %s\n", (e1 == HALTZ_OK && e2 == HALTZ_OK) ? "sent" : "not sent (link down?)");
}

static const char *const k_sfx_names[HALTZ_IO_SFX_COUNT] = {
    "BEEP", "CLICK", "KEY",  "OK",   "ERROR", "COIN", "JUMP",
    "POWER_UP", "POWER_DOWN", "BOOT", "CART", "HALT", "HIT"};

static void next_sfx(void) {
    int id = s_sfx_next;
    s_sfx_next = (s_sfx_next + 1) % HALTZ_IO_SFX_COUNT;
    haltz_err_t e = io_sfx((uint8_t)id, HALTZ_IO_VOICE_A);
    printf("io: sound effect %d %s%s\n", id, k_sfx_names[id], e == HALTZ_OK ? "" : " (link down)");
}

static void next_led(void) {
    static const struct {
        const char *name;
        haltz_io_led_t m;
    } k[] = {
        {"breathe blue (booting)", {HALTZ_IO_LED_BREATHE, 0, 0, 255, 2000, 0}},
        {"blink blue (no keyboard)", {HALTZ_IO_LED_BLINK, 0, 0, 255, 800, 0}},
        {"green (running)", {HALTZ_IO_LED_SOLID, 0, 160, 0, 0, 0}},
        {"flash white (cartridge)", {HALTZ_IO_LED_FLASH, 255, 255, 255, 150, 0}},
        {"red (halted)", {HALTZ_IO_LED_SOLID, 255, 0, 0, 0, 0}},
        {"red code 3 (error 3)", {HALTZ_IO_LED_CODE, 255, 0, 0, 300, 3}},
        {"fade to black (shutdown)", {HALTZ_IO_LED_FADE, 0, 0, 0, 1500, 0}},
        {"orange breathe", {HALTZ_IO_LED_BREATHE, 255, 90, 0, 1200, 0}},
    };
    int i = s_led_next;
    s_led_next = (s_led_next + 1) % (int)(sizeof(k) / sizeof(k[0]));
    const haltz_io_led_t *m = &k[i].m;
    io_led(m->mode, m->r, m->g, m->b, m->period_ms, m->count);
    printf("io: LED %s\n", k[i].name);
}

/* ---- link test --------------------------------------------------------------------------- */

static bool fresh_status(io_stats_t *st, uint32_t newer_than_ms) {
    for (int i = 0; i < 100; i++) {
        io_get_stats(st);
        if (st->have_status && st->status_age_ms < newer_than_ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void link_test(void) {
    if (!io_is_up()) {
        printf("io: link test needs the link up\n");
        return;
    }
    printf("io: link test, %d LED commands...\n", LINK_TEST_CMDS);
    io_stats_t a, b;
    vTaskDelay(pdMS_TO_TICKS(300)); /* a status from after anything sent before */
    if (!fresh_status(&a, 250)) {
        printf("io: link test: no IO_STATUS\n");
        return;
    }
    int64_t t0 = now_ms();
    uint32_t refused = 0;
    for (int i = 0; i < LINK_TEST_CMDS; i++) {
        uint32_t r = esp_random();
        if (io_led(HALTZ_IO_LED_SOLID, (uint8_t)r, (uint8_t)(r >> 8), (uint8_t)(r >> 16), 0, 0) !=
            HALTZ_OK) {
            refused++;
        }
    }
    int64_t t1 = now_ms();
    vTaskDelay(pdMS_TO_TICKS(600)); /* the UART drains, then two IO_STATUS go by */
    bool got = fresh_status(&b, 250);
    led_policy(s_had_keyboard);

    uint16_t sent = (uint16_t)(b.tx_frames - a.tx_frames);
    uint16_t counted = (uint16_t)(b.status.rx_frames - a.status.rx_frames);
    uint16_t crc = (uint16_t)(b.status.crc_errors - a.status.crc_errors);
    uint16_t nak = (uint16_t)(b.status.cmd_errors - a.status.cmd_errors);
    uint16_t ovr = (uint16_t)(b.status.rx_overruns - a.status.rx_overruns);
    uint32_t s3_bad = (b.rx_crc_errors - a.rx_crc_errors) + (b.rx_bad - a.rx_bad);
    uint32_t ms = (uint32_t)(t1 - t0);
    bool pass = got && refused == 0 && sent == counted && crc == 0 && nak == 0 && ovr == 0 &&
                s3_bad == 0 && b.resets == a.resets && b.state == IO_LINK_UP;
    printf(" sent %u frames in %" PRIu32 " ms, board counted %u\n", sent, ms, counted);
    printf(" board: crc %u, NAKs %u, overruns %u\n", crc, nak, ovr);
    printf(" S3: bad frames %" PRIu32 ", resets %" PRIu32 "\n", s3_bad, b.resets - a.resets);
    printf("io: link test %s\n", pass ? "PASS" : "FAIL");
}

/* ---- events ------------------------------------------------------------------------------ */

static void echo_key(const io_key_event_t *k) {
    if (!k->down) {
        return;
    }
    if (k->ch == '\b') {
        printf("\b \b");
    } else if (k->ch == '\n' || k->ch == '\t' || (k->ch >= 0x20 && k->ch < 0x7F)) {
        putchar(k->ch);
    } else if (k->usage >= HALTZ_KEY_F1 && k->usage <= HALTZ_KEY_UP && k->ch == 0) {
        printf("<%s>", io_key_name(k->usage)); /* F-keys, arrows, Home, ... */
    } else {
        return; /* modifiers, Escape, Ctrl+letter: nothing to show */
    }
    fflush(stdout);
}

bool io_bringup_poll(bool draw) {
    bool changed = false;
    io_event_t ev;
    while (io_get_event(&ev, 0)) {
        switch (ev.type) {
        case IO_EV_KEY:
            echo_key(&ev.u.key);
            break;
        case IO_EV_STICK:
            if (ev.u.stick.pressed & HALTZ_IO_BTN_STICK) {
                io_sfx(HALTZ_IO_SFX_CLICK, HALTZ_IO_VOICE_B);
            }
            if (ev.u.stick.pressed || ev.u.stick.released) {
                printf("io: buttons %u\n", ev.u.stick.now.buttons);
            }
            break;
        case IO_EV_DEVICE: {
            static const char *const kinds[] = {"device", "keyboard", "mouse", "gamepad"};
            const haltz_io_device_t *d = &ev.u.device;
            printf("io: %s %s (slot %u, battery %u)\n", kinds[d->kind],
                   d->state ? "connected" : "disconnected", d->slot, d->battery);
            break;
        }
        case IO_EV_LINK:
            if (ev.u.link_up) {
                io_sfx(HALTZ_IO_SFX_BOOT, HALTZ_IO_VOICE_A);
            } else {
                printf("io: link down\n");
            }
            break;
        default:
            break;
        }
    }

    io_input_t in;
    io_get_input(&in);
    bool up = io_is_up();
    if (up && (!s_was_up || in.keyboard != s_had_keyboard)) {
        led_policy(in.keyboard);
    }
    s_was_up = up;
    s_had_keyboard = in.keyboard;
    if (draw) {
        changed |= move_pointer(&in.stick);
    } else {
        s_last_move_us = 0;
    }
    return changed;
}

/* ---- keys -------------------------------------------------------------------------------- */

void io_bringup_help(void) {
    printf("I/O keys: i stats, p tune, b sound,\n      l LED, c stick centre, e reset IO,\n"
           "      k link test\n");
}

void io_bringup_stats(void) {
    io_stats_t st;
    io_get_stats(&st);
    printf("I/O link %s, %d baud\n", io_link_state_name(st.state), HALTZ_IO_BAUD);
    printf(" board protocol v%u, git %s\n", (unsigned)st.peer_proto, st.peer_git);
    printf(" resets %" PRIu32 ", link ups %" PRIu32 ", reboots %" PRIu32 "\n", st.resets,
           st.link_ups, st.reboots);
    printf(" last reset: %s\n", st.last_reset_reason ? st.last_reset_reason : "-");
    printf(" rx %" PRIu32 " frames, crc %" PRIu32 ", bad %" PRIu32 "\n", st.rx_frames,
           st.rx_crc_errors, st.rx_bad);
    printf(" tx %" PRIu32 " frames, %" PRIu64 " bytes\n", st.tx_frames, st.tx_bytes);
    printf(" NAKs %" PRIu32 ", events dropped %" PRIu32 "\n", st.naks, st.events_dropped);
    if (st.have_status) {
        const haltz_io_status_t *s = &st.status;
        printf("IO_STATUS (%" PRIu32 " ms old), up %" PRIu32 " s\n", st.status_age_ms,
               s->uptime_ms / 1000);
        printf(" rx %u, crc %u, NAKs %u, ovr %u\n", s->rx_frames, s->crc_errors, s->cmd_errors,
               s->rx_overruns);
        printf(" flags %02X, devices %02X, free %u/%u\n", s->flags, s->devices, s->tune_free[0],
               s->tune_free[1]);
    } else {
        printf("no IO_STATUS yet\n");
    }
}

bool io_bringup_key(int c) {
    switch (c) {
    case 'i':
        io_bringup_stats();
        return true;
    case 'p':
        play_test_tune();
        return true;
    case 'b':
        next_sfx();
        return true;
    case 'l':
        next_led();
        return true;
    case 'c':
        printf("io: measuring the stick centre (hands off)\n");
        io_stick_calibrate();
        return true;
    case 'e':
        printf("io: resetting the I/O board on request\n");
        io_force_reset();
        return true;
    case 'k':
        link_test();
        return true;
    default:
        return false;
    }
}

/* ---- setup ------------------------------------------------------------------------------- */

void io_bringup_redraw(void) {
    load_pointer();
}

void io_bringup_init(int row) {
    s_row = row;
    load_pointer();
}
