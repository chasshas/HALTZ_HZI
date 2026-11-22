/*
 * HALTZ I/O: console commands, added to Bluepad32's console on the USB port (115200).
 * For the board alone (architecture-v1 bring-up step 5) and for looking at the link.
 *   io            link and input state
 *   io_watch      print every input message sent and command received (again: off)
 *   io_test       LED colours, every sound effect, a two-voice tune (no S3 needed)
 *   bt_scan on|off, bt_forget
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "haltz/io.h"
#include "haltz/msgs.h"
#include "haltz_platform.h"
#include "io_fx.h"
#include "io_input.h"
#include "io_link.h"
#include "tunes.h"

static const char *const k_kind[] = {"other", "keyboard", "mouse", "gamepad"};

static int cmd_io(int argc, char **argv) {
    (void)argc;
    (void)argv;
    io_link_stats_t st;
    io_link_get_stats(&st);
    const haltz_io_status_t *s = &st.status;
    const input_state_t *in = input_state();
    printf("HALTZ IO, protocol v%u, up %" PRIu32 " s\n", (unsigned)HALTZ_PROTO_VERSION,
           st.uptime_ms / 1000);
    if (st.s3_hellos == 0) {
        printf(" S3: no HELLO yet\n");
    } else {
        printf(" S3: protocol v%u git %s, %" PRIu32 " HELLOs, last frame %" PRIu32 " ms ago\n",
               st.s3_proto, st.s3_git, st.s3_hellos, st.last_rx_age_ms);
    }
    printf(" rx %u frames, crc %u, NAKs %u, overruns %u\n", s->rx_frames, s->crc_errors,
           s->cmd_errors, s->rx_overruns);
    printf(" tx %" PRIu32 " frames, %" PRIu32 " bytes; Bluetooth events dropped %" PRIu32 "\n",
           st.tx_frames, st.tx_bytes, st.bt_dropped);
    printf(" stick x %+5d y %+5d buttons %u (raw %d %d, centre %d %d %s)\n", in->stick.x,
           in->stick.y, in->stick.buttons, in->raw_x, in->raw_y, in->centre_x, in->centre_y,
           in->stick_ok ? "OK" : "BAD");
    printf(" knob %u (raw %d)\n", in->knob, in->raw_knob);
    printf(" keys mods %02X:", st.keys.modifiers);
    for (int i = 0; i < st.keys.count; i++) {
        printf(" %02X", st.keys.usages[i]);
    }
    printf("\n Bluetooth %s\n", (s->flags & HALTZ_IO_STATUS_SCANNING) ? "scanning" : "not scanning");
    for (int i = 0; i < HALTZ_IO_MAX_DEVICES; i++) {
        if (s->devices & (1u << i)) {
            printf("  slot %d: %s, battery %u\n", i,
                   k_kind[st.device_kind[i] <= HALTZ_IO_DEV_KIND_LAST ? st.device_kind[i] : 0],
                   st.device_battery[i]);
        }
    }
    printf(" voices %s%s, tune free %u / %u\n", (s->flags & HALTZ_IO_STATUS_VOICE_A) ? "A" : "-",
           (s->flags & HALTZ_IO_STATUS_VOICE_B) ? "B" : "-", s->tune_free[0], s->tune_free[1]);
    return 0;
}

static int cmd_io_watch(int argc, char **argv) {
    bool on = !io_link_watch();
    if (argc > 1) {
        on = strcmp(argv[1], "off") != 0;
    }
    io_link_set_watch(on);
    printf("io_watch %s\n", on ? "on" : "off");
    return 0;
}

static void led(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period, uint8_t count) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_LED};
    c.u.led = (haltz_io_led_t){mode, r, g, b, period, count};
    fx_cmd(&c);
}

static void notes(uint8_t voice, uint8_t flags, uint8_t tick_ms, const uint8_t *nt, int n) {
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_NOTES};
    c.u.notes.voice = voice;
    c.u.notes.flags = flags;
    c.u.notes.tick_ms = tick_ms;
    c.u.notes.count = (uint8_t)(n / 2);
    for (int i = 0; i < n / 2; i++) {
        c.u.notes.notes[i] = (haltz_io_note_t){nt[2 * i], nt[2 * i + 1]};
    }
    fx_cmd(&c);
}

static int cmd_io_test(int argc, char **argv) {
    (void)argc;
    (void)argv;
    static const struct {
        const char *name;
        uint8_t r, g, b;
    } colours[] = {{"red", 255, 0, 0},     {"green", 0, 255, 0}, {"blue", 0, 0, 255},
                   {"white", 255, 255, 255}, {"half white", 128, 128, 128}};
    for (size_t i = 0; i < sizeof(colours) / sizeof(colours[0]); i++) {
        printf("LED %s\n", colours[i].name);
        led(HALTZ_IO_LED_SOLID, colours[i].r, colours[i].g, colours[i].b, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    printf("LED: blink blue, breathe green, red code 3, flash white, fade out\n");
    led(HALTZ_IO_LED_BLINK, 0, 0, 255, 400, 0);
    vTaskDelay(pdMS_TO_TICKS(1600));
    led(HALTZ_IO_LED_BREATHE, 0, 255, 0, 1500, 0);
    vTaskDelay(pdMS_TO_TICKS(3000));
    led(HALTZ_IO_LED_CODE, 255, 0, 0, 300, 3);
    vTaskDelay(pdMS_TO_TICKS(1800));
    led(HALTZ_IO_LED_FLASH, 255, 255, 255, 150, 0);
    vTaskDelay(pdMS_TO_TICKS(1200));
    led(HALTZ_IO_LED_FADE, 0, 0, 0, 1000, 0);
    vTaskDelay(pdMS_TO_TICKS(1200));

    for (uint8_t id = 0; id < HALTZ_IO_SFX_COUNT; id++) {
        printf("sound effect %u %s\n", id, seq_sfx_name(id));
        haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_SFX};
        c.u.sfx = (haltz_io_sfx_msg_t){.id = id, .voice = id % 2};
        fx_cmd(&c);
        vTaskDelay(pdMS_TO_TICKS(seq_sfx_ms(id) + 300));
    }

    printf("two voices: scale on A, bass on B\n");
    static const uint8_t melody[] = {72, 2, 74, 2, 76, 2, 77, 2, 79, 2, 81, 2, 83, 2, 84, 4,
                                     83, 2, 81, 2, 79, 2, 77, 2, 76, 2, 74, 2, 72, 6};
    static const uint8_t bass[] = {48, 8, 53, 8, 55, 8, 48, 8};
    notes(HALTZ_IO_VOICE_A, HALTZ_IO_NOTES_REPLACE, 125, melody, sizeof(melody));
    notes(HALTZ_IO_VOICE_B, HALTZ_IO_NOTES_REPLACE, 125, bass, sizeof(bass));
    vTaskDelay(pdMS_TO_TICKS(4500));
    printf("same with an effect on top of A (the scale pauses, then goes on)\n");
    notes(HALTZ_IO_VOICE_A, HALTZ_IO_NOTES_REPLACE, 125, melody, sizeof(melody));
    notes(HALTZ_IO_VOICE_B, HALTZ_IO_NOTES_REPLACE, 125, bass, sizeof(bass));
    vTaskDelay(pdMS_TO_TICKS(1100));
    haltz_io_cmd_t c = {.type = HALTZ_MSG_IO_SFX};
    c.u.sfx = (haltz_io_sfx_msg_t){.id = HALTZ_IO_SFX_COIN, .voice = HALTZ_IO_VOICE_A};
    fx_cmd(&c);
    vTaskDelay(pdMS_TO_TICKS(4500));
    led(HALTZ_IO_LED_BREATHE, 0, 0, 255, 2000, 0);
    printf("io_test done\n");
    return 0;
}

static int cmd_bt_scan(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        printf("bt_scan on|off (now %s)\n", haltz_bt_scanning() ? "on" : "off");
        return 1;
    }
    haltz_bt_action(strcmp(argv[1], "on") == 0 ? HALTZ_IO_BT_SCAN_ON : HALTZ_IO_BT_SCAN_OFF);
    return 0;
}

static int cmd_bt_forget(int argc, char **argv) {
    (void)argc;
    (void)argv;
    haltz_bt_action(HALTZ_IO_BT_FORGET);
    printf("forgetting every paired device; put the keyboard in pairing mode\n");
    return 0;
}

/* Called by Bluepad32 when its console starts (haltz_platform.c). */
void io_console_register(void) {
    const esp_console_cmd_t cmds[] = {
        {.command = "io", .help = "HALTZ: link and input state", .func = cmd_io},
        {.command = "io_watch",
         .help = "HALTZ: print input messages and commands [on|off]",
         .func = cmd_io_watch},
        {.command = "io_test",
         .help = "HALTZ: LED colours, every sound effect, a two-voice tune",
         .func = cmd_io_test},
        {.command = "bt_scan", .help = "HALTZ: Bluetooth scanning on|off", .func = cmd_bt_scan},
        {.command = "bt_forget",
         .help = "HALTZ: forget every paired device",
         .func = cmd_bt_forget},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
}
