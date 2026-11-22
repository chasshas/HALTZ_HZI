/*
 * HALTZ CPU: driver for the I/O link to the ESP32 I/O board (docs/io-protocol-v1.md, S3 side:
 * section 9).
 *
 * Use:
 *     io_start();                                  once, at boot: UART1, EN, driver task
 *     io_event_t ev;
 *     while (io_get_event(&ev, 0)) { ... }         keys, stick, knob, devices, link up/down
 *     io_led(HALTZ_IO_LED_SOLID, 0, 255, 0, 0, 0);  green
 *     io_sfx(HALTZ_IO_SFX_BEEP, HALTZ_IO_VOICE_A);
 *
 * - The command calls may be used from any task. They check the values with the shared pack
 *   rules (HALTZ_ERR_BAD_VALUE: nothing is sent) and write the frame at once (the UART driver
 *   buffers it; they block only if 2 KB are already waiting).
 * - While the link is down, LED, volume and Bluetooth calls only change the state the driver
 *   keeps; it is sent when the link comes up. Sound calls return HALTZ_ERR_BUSY.
 * - The driver task (core 0) does the handshake, watches the board's IO_STATUS and resets the
 *   board (EN) when nothing arrives for 1.5 s. Resetting the board drops the Bluetooth
 *   keyboard, so it happens only then, at boot and on io_force_reset().
 */
#ifndef HALTZ_CPU_IO_H
#define HALTZ_CPU_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "haltz/io.h"
#include "haltz/io_msgs.h"
#include "haltz/proto.h"
#include "io_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IO_LINK_RESET,     /* holding / releasing EN */
    IO_LINK_HANDSHAKE, /* HELLO every 100 ms, waiting for the board's HELLO */
    IO_LINK_UP,
    IO_LINK_MISSING, /* 3 resets without an answer; tried again every 10 s */
} io_link_state_t;

typedef enum {
    IO_EV_KEY,    /* u.key */
    IO_EV_STICK,  /* u.stick: the new state, and which buttons went down / up */
    IO_EV_KNOB,   /* u.knob */
    IO_EV_DEVICE, /* u.device */
    IO_EV_LINK,   /* u.link_up */
} io_event_type_t;

typedef struct {
    uint8_t type; /* io_event_type_t */
    union {
        io_key_event_t key;
        struct {
            haltz_io_stick_t now;
            uint8_t pressed, released; /* HALTZ_IO_BTN_* */
        } stick;
        uint8_t knob;
        haltz_io_device_t device;
        bool link_up;
    } u;
} io_event_t;

/* The latest input state (what the events led to). */
typedef struct {
    haltz_io_keys_t keys;
    bool caps_lock;
    haltz_io_stick_t stick;
    uint8_t knob;
    uint8_t devices; /* bit n = Bluetooth slot n connected */
    bool keyboard;   /* a keyboard is connected */
    bool stick_ok;   /* the board measured a plausible stick centre */
} io_input_t;

typedef struct {
    io_link_state_t state;
    uint16_t peer_proto; /* the board's HELLO; 0 = none yet */
    char peer_git[9];
    uint32_t resets, link_ups, reboots; /* reboots: the board restarted by itself */
    const char *last_reset_reason;      /* NULL if never */
    uint32_t rx_frames, rx_crc_errors, rx_bad; /* bad = refused by unpack */
    uint32_t naks;                      /* NAKs from the board */
    uint32_t tx_frames;
    uint64_t tx_bytes;
    uint32_t events_dropped; /* event queue full */
    bool have_status;
    haltz_io_status_t status; /* the board's last IO_STATUS */
    uint32_t status_age_ms;
} io_stats_t;

/* ---- driver ------------------------------------------------------------------------------ */

/* UART1, the EN pin, the event queue and the driver task. Returns at once. Call once. */
esp_err_t io_start(void);
bool io_is_up(void);
bool io_wait_up(uint32_t timeout_ms);
void io_get_stats(io_stats_t *out);
const char *io_link_state_name(io_link_state_t s);
/* Reset the board now (drops the keyboard); the driver redoes the handshake. */
void io_force_reset(void);

/* ---- input ------------------------------------------------------------------------------- */

/* Next event; waits up to timeout_ms (0 = don't wait). False if none. */
bool io_get_event(io_event_t *ev, uint32_t timeout_ms);
void io_get_input(io_input_t *out);

/* ---- commands (protocol section 6). All return HALTZ_OK or the pack error. --------------- */

haltz_err_t io_cmd(const haltz_io_cmd_t *cmd);
haltz_err_t io_led(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms,
                   uint8_t count);
haltz_err_t io_tone(uint8_t voice, uint16_t hz, uint16_t ms);
/* A tune of any length up to HALTZ_IO_TUNE_NOTES: split into IO_NOTES messages, REPLACE (if
 * in flags) only on the first. Every message is checked before the first one is sent. */
haltz_err_t io_notes(uint8_t voice, uint8_t flags, uint8_t tick_ms, const haltz_io_note_t *notes,
                     size_t n);
haltz_err_t io_sfx(uint8_t id, uint8_t voice);
haltz_err_t io_stop(uint8_t voices);
haltz_err_t io_volume(uint8_t voices, uint8_t level);
haltz_err_t io_stick_calibrate(void);
haltz_err_t io_bt(uint8_t action);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_IO_H */
