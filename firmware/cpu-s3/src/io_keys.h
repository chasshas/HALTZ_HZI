/*
 * HALTZ CPU: keyboard states -> key events and characters (docs/io-protocol-v1.md
 * sections 3 and 9). Portable C, no ESP-IDF calls: the PC test test/test_io_keys.c uses it.
 *
 * IO_KEYS carries the whole keyboard state. io_keys_diff() compares two states and reports
 * every change as a key event, with the character a US keyboard types (Shift, Caps Lock and
 * Ctrl applied).
 */
#ifndef HALTZ_CPU_IO_KEYS_H
#define HALTZ_CPU_IO_KEYS_H

#include <stdbool.h>
#include <stdint.h>

#include "haltz/io.h"
#include "haltz/io_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t usage;     /* HID usage; modifier keys as 0xE0..0xE7 */
    bool down;         /* pressed (true) or released */
    uint8_t modifiers; /* HALTZ_IO_MOD_* after this change */
    char ch;           /* key down only: the character, 0 if the key types none */
} io_key_event_t;

typedef void (*io_key_cb_t)(const io_key_event_t *ev, void *ctx);

/*
 * Report every difference between prev and next, in this order: keys released, modifiers
 * released, modifiers pressed, keys pressed (so Shift+A in one report types 'A'). Caps Lock
 * toggles *caps_lock when it goes down. Every event carries next's modifiers.
 */
void io_keys_diff(const haltz_io_keys_t *prev, const haltz_io_keys_t *next, bool *caps_lock,
                  io_key_cb_t cb, void *ctx);

/* The character for a key (US layout), 0 if none. Ctrl+letter gives 1..26 (Ctrl+A = 1).
 * Enter '\n', Backspace '\b', Tab '\t', Escape 0x1B, Delete 0x7F, keypad digits as digits. */
char io_key_char(uint8_t usage, uint8_t modifiers, bool caps_lock);

/* Short name for logs: "A", "Enter", "F1", "Up", "LShift", ... ("?" if unknown). */
const char *io_key_name(uint8_t usage);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_IO_KEYS_H */
