/*
 * HALTZ I/O link: constants and bit layouts for the IO_* messages in msg_ids.def.
 * The protocol is described in docs/io-protocol-v1.md; section numbers below refer to it.
 * Values, bits and IDs are part of the protocol. Link timings are policy of the two boards.
 */
#ifndef HALTZ_IO_H
#define HALTZ_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. Transport (UART, S3 UART1 <-> ESP32 UART2) -------------------------------------- */
#define HALTZ_IO_BAUD          115200 /* 8N1, no flow control */
#define HALTZ_IO_IDLE_FLUSH_MS 10     /* no byte this long inside a frame: haltz_decoder_flush() */
#define HALTZ_IO_RESET_MS      20     /* EN held low at least this long */

/* ---- 2. Handshake and health ------------------------------------------------------------- */
#define HALTZ_IO_HELLO_EVERY_MS   100  /* S3 repeats HELLO until the I/O board answers */
#define HALTZ_IO_HELLO_TIMEOUT_MS 3000 /* no HELLO back: reset again */
#define HALTZ_IO_HELLO_TRIES      3    /* then: I/O board missing */
#define HALTZ_IO_STATUS_EVERY_MS  250  /* I/O board sends IO_STATUS + the input refresh */
#define HALTZ_IO_DEAD_MS          1500 /* nothing received this long: reset */

/* ---- 3. Input ----------------------------------------------------------------------------- */
#define HALTZ_IO_KEYS_MAX    10   /* keys (besides modifiers) in one IO_KEYS */
#define HALTZ_IO_USAGE_FIRST 0x04 /* HID keyboard page: 0x04 = A */
#define HALTZ_IO_USAGE_LAST  0xDF /* 0xE0-0xE7 are the modifiers, sent as bits */
#define HALTZ_IO_AXIS_MAX    1000 /* stick x, y in -1000..1000; right = +x, up = +y */
#define HALTZ_IO_MAX_DEVICES 4    /* Bluetooth device slots */

/* IO_KEYS modifiers (HID standard order; usage 0xE0 + bit number) */
#define HALTZ_IO_MOD_LCTRL  0x01u
#define HALTZ_IO_MOD_LSHIFT 0x02u
#define HALTZ_IO_MOD_LALT   0x04u
#define HALTZ_IO_MOD_LGUI   0x08u
#define HALTZ_IO_MOD_RCTRL  0x10u
#define HALTZ_IO_MOD_RSHIFT 0x20u
#define HALTZ_IO_MOD_RALT   0x40u
#define HALTZ_IO_MOD_RGUI   0x80u
#define HALTZ_IO_MOD_CTRL   (HALTZ_IO_MOD_LCTRL | HALTZ_IO_MOD_RCTRL)
#define HALTZ_IO_MOD_SHIFT  (HALTZ_IO_MOD_LSHIFT | HALTZ_IO_MOD_RSHIFT)
#define HALTZ_IO_MOD_ALT    (HALTZ_IO_MOD_LALT | HALTZ_IO_MOD_RALT)
#define HALTZ_IO_MOD_GUI    (HALTZ_IO_MOD_LGUI | HALTZ_IO_MOD_RGUI)
#define HALTZ_IO_USAGE_MOD_FIRST 0xE0 /* left Ctrl; the S3 reports modifier changes as keys */

/* A few HID usages the kernel cares about (keyboard page 0x07). */
enum {
    HALTZ_KEY_A = 0x04,
    HALTZ_KEY_Z = 0x1D,
    HALTZ_KEY_1 = 0x1E,
    HALTZ_KEY_0 = 0x27,
    HALTZ_KEY_ENTER = 0x28,
    HALTZ_KEY_ESCAPE = 0x29,
    HALTZ_KEY_BACKSPACE = 0x2A,
    HALTZ_KEY_TAB = 0x2B,
    HALTZ_KEY_SPACE = 0x2C,
    HALTZ_KEY_CAPS_LOCK = 0x39,
    HALTZ_KEY_F1 = 0x3A,
    HALTZ_KEY_F12 = 0x45,
    HALTZ_KEY_INSERT = 0x49,
    HALTZ_KEY_HOME = 0x4A,
    HALTZ_KEY_PAGE_UP = 0x4B,
    HALTZ_KEY_DELETE = 0x4C,
    HALTZ_KEY_END = 0x4D,
    HALTZ_KEY_PAGE_DOWN = 0x4E,
    HALTZ_KEY_RIGHT = 0x4F,
    HALTZ_KEY_LEFT = 0x50,
    HALTZ_KEY_DOWN = 0x51,
    HALTZ_KEY_UP = 0x52,
};

/* IO_STICK buttons */
#define HALTZ_IO_BTN_STICK 0x01u /* stick click (GPIO32) */
#define HALTZ_IO_BTN_SPARE 0x02u /* spare push button (GPIO33), not wired yet */
#define HALTZ_IO_BTN_ALL   0x03u /* other bits must be 0 */

/* IO_DEVICE */
typedef enum {
    HALTZ_IO_DEV_OTHER = 0,
    HALTZ_IO_DEV_KEYBOARD = 1,
    HALTZ_IO_DEV_MOUSE = 2,
    HALTZ_IO_DEV_GAMEPAD = 3,
} haltz_io_dev_kind_t;
#define HALTZ_IO_DEV_KIND_LAST HALTZ_IO_DEV_GAMEPAD
#define HALTZ_IO_DEV_DISCONNECTED 0
#define HALTZ_IO_DEV_CONNECTED    1
#define HALTZ_IO_BATTERY_UNKNOWN  0

/* IO_STATUS flags */
#define HALTZ_IO_STATUS_KEYBOARD 0x01u /* a keyboard is connected */
#define HALTZ_IO_STATUS_SCANNING 0x02u /* Bluetooth scanning: new devices can connect */
#define HALTZ_IO_STATUS_VOICE_A  0x04u /* voice A is sounding */
#define HALTZ_IO_STATUS_VOICE_B  0x08u /* voice B is sounding */
#define HALTZ_IO_STATUS_STICK_OK 0x10u /* stick centre measured and plausible */

/* ---- 4. Sound ----------------------------------------------------------------------------- */
#define HALTZ_IO_VOICES      2
#define HALTZ_IO_VOICE_A     0
#define HALTZ_IO_VOICE_B     1
#define HALTZ_IO_VOICES_ALL  0x03u /* voice mask: bit 0 = A, bit 1 = B */
#define HALTZ_IO_TUNE_NOTES  255   /* tune buffer per voice */
#define HALTZ_IO_NOTE_REST   0
#define HALTZ_IO_NOTE_LOWEST 12    /* MIDI C0, 16.4 Hz */
#define HALTZ_IO_NOTE_HIGHEST 127  /* MIDI G9, 12.5 kHz */
#define HALTZ_IO_NOTE_A4     69    /* 440 Hz */
#define HALTZ_IO_TONE_HZ_MIN 20    /* IO_TONE hz: 0 or 20..20000 */
#define HALTZ_IO_TONE_HZ_MAX 20000
#define HALTZ_IO_VOLUME_FULL 255

/* IO_NOTES flags */
#define HALTZ_IO_NOTES_REPLACE 0x01u /* empty the tune buffer first */
#define HALTZ_IO_NOTES_LOOP    0x02u /* the tune repeats */
#define HALTZ_IO_NOTES_LEGATO  0x04u /* no gap after these notes */
#define HALTZ_IO_NOTES_FLAGS   0x07u /* other bits must be 0 */

/* IO_SFX ids. The sounds are defined in the I/O firmware (tunes.c). */
typedef enum {
    HALTZ_IO_SFX_BEEP = 0,
    HALTZ_IO_SFX_CLICK = 1,
    HALTZ_IO_SFX_KEY = 2,
    HALTZ_IO_SFX_OK = 3,
    HALTZ_IO_SFX_ERROR = 4,
    HALTZ_IO_SFX_COIN = 5,
    HALTZ_IO_SFX_JUMP = 6,
    HALTZ_IO_SFX_POWER_UP = 7,
    HALTZ_IO_SFX_POWER_DOWN = 8,
    HALTZ_IO_SFX_BOOT = 9,
    HALTZ_IO_SFX_CART = 10,
    HALTZ_IO_SFX_HALT = 11,
    HALTZ_IO_SFX_HIT = 12,
    HALTZ_IO_SFX_COUNT
} haltz_io_sfx_t;

/* ---- 5. LED ------------------------------------------------------------------------------- */
typedef enum {
    HALTZ_IO_LED_SOLID = 0,
    HALTZ_IO_LED_BLINK = 1,
    HALTZ_IO_LED_BREATHE = 2,
    HALTZ_IO_LED_FLASH = 3,
    HALTZ_IO_LED_FADE = 4,
    HALTZ_IO_LED_CODE = 5,
} haltz_io_led_mode_t;
#define HALTZ_IO_LED_MODE_LAST     HALTZ_IO_LED_CODE
#define HALTZ_IO_LED_PERIOD_MIN    20 /* BLINK, BREATHE, CODE */
#define HALTZ_IO_LED_CODE_MAX      15 /* CODE blinks */
#define HALTZ_IO_LED_CODE_PAUSE    3  /* CODE: off for this many periods after the blinks */

/* ---- IO_BT actions ----------------------------------------------------------------------- */
typedef enum {
    HALTZ_IO_BT_SCAN_OFF = 0,
    HALTZ_IO_BT_SCAN_ON = 1,
    HALTZ_IO_BT_FORGET = 2,
} haltz_io_bt_action_t;
#define HALTZ_IO_BT_ACTION_LAST HALTZ_IO_BT_FORGET

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_H */
