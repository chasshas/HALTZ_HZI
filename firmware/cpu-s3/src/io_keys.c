/* HALTZ CPU: keyboard states -> key events and characters. See io_keys.h. */
#include "io_keys.h"

#include <stddef.h>

static bool has(const haltz_io_keys_t *k, uint8_t usage) {
    for (int i = 0; i < k->count; i++) {
        if (k->usages[i] == usage) {
            return true;
        }
    }
    return false;
}

static void emit(io_key_cb_t cb, void *ctx, uint8_t usage, bool down, uint8_t mods,
                 bool caps_lock) {
    io_key_event_t ev = {.usage = usage, .down = down, .modifiers = mods, .ch = 0};
    if (down) {
        ev.ch = io_key_char(usage, mods, caps_lock);
    }
    cb(&ev, ctx);
}

void io_keys_diff(const haltz_io_keys_t *prev, const haltz_io_keys_t *next, bool *caps_lock,
                  io_key_cb_t cb, void *ctx) {
    uint8_t mods = next->modifiers;
    for (int i = 0; i < prev->count; i++) {
        if (!has(next, prev->usages[i])) {
            emit(cb, ctx, prev->usages[i], false, mods, *caps_lock);
        }
    }
    uint8_t released = (uint8_t)(prev->modifiers & ~next->modifiers);
    uint8_t pressed = (uint8_t)(next->modifiers & ~prev->modifiers);
    for (int b = 0; b < 8; b++) {
        if (released & (1u << b)) {
            emit(cb, ctx, (uint8_t)(HALTZ_IO_USAGE_MOD_FIRST + b), false, mods, *caps_lock);
        }
    }
    for (int b = 0; b < 8; b++) {
        if (pressed & (1u << b)) {
            emit(cb, ctx, (uint8_t)(HALTZ_IO_USAGE_MOD_FIRST + b), true, mods, *caps_lock);
        }
    }
    for (int i = 0; i < next->count; i++) {
        if (!has(prev, next->usages[i])) {
            if (next->usages[i] == HALTZ_KEY_CAPS_LOCK) {
                *caps_lock = !*caps_lock;
            }
            emit(cb, ctx, next->usages[i], true, mods, *caps_lock);
        }
    }
}

/* US layout, usages 0x1E..0x38: digits row and punctuation. */
static const char k_plain[] = "1234567890\n\x1b\b\t -=[]\\\0;'`,./";
static const char k_shift[] = "!@#$%^&*()\n\x1b\b\t _+{}|\0:\"~<>?";
_Static_assert(sizeof(k_plain) == 0x38 - 0x1E + 2, "digits/punctuation table size");
_Static_assert(sizeof(k_shift) == sizeof(k_plain), "shifted table size");

char io_key_char(uint8_t usage, uint8_t modifiers, bool caps_lock) {
    bool shift = (modifiers & HALTZ_IO_MOD_SHIFT) != 0;
    bool ctrl = (modifiers & HALTZ_IO_MOD_CTRL) != 0;
    if (usage >= HALTZ_KEY_A && usage <= HALTZ_KEY_Z) {
        int i = usage - HALTZ_KEY_A;
        if (ctrl) {
            return (char)(1 + i);
        }
        return (char)((shift != caps_lock) ? 'A' + i : 'a' + i);
    }
    if (usage >= 0x1E && usage <= 0x38) {
        return (shift ? k_shift : k_plain)[usage - 0x1E];
    }
    switch (usage) {
    case HALTZ_KEY_DELETE:
        return 0x7F;
    case 0x54:
        return '/';
    case 0x55:
        return '*';
    case 0x56:
        return '-';
    case 0x57:
        return '+';
    case 0x58:
        return '\n'; /* keypad Enter */
    case 0x62:
        return '0';
    case 0x63:
        return '.';
    default:
        break;
    }
    if (usage >= 0x59 && usage <= 0x61) {
        return (char)('1' + (usage - 0x59)); /* keypad 1-9 (Num Lock assumed on) */
    }
    return 0;
}

const char *io_key_name(uint8_t usage) {
    static const char *const letters[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I",
                                            "J", "K", "L", "M", "N", "O", "P", "Q", "R",
                                            "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char *const row[] = {
        /* 0x1E */ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
        /* 0x28 */ "Enter", "Esc", "Backspace", "Tab", "Space", "-", "=", "[", "]", "\\",
        /* 0x32 */ "#", ";", "'", "`", ",", ".", "/", "CapsLock",
        /* 0x3A */ "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        /* 0x46 */ "PrtSc", "ScrLock", "Pause", "Insert", "Home", "PgUp", "Delete", "End",
        /* 0x4E */ "PgDn", "Right", "Left", "Down", "Up"};
    static const char *const mods[8] = {"LCtrl", "LShift", "LAlt", "LGUI",
                                        "RCtrl", "RShift", "RAlt", "RGUI"};
    if (usage >= HALTZ_KEY_A && usage <= HALTZ_KEY_Z) {
        return letters[usage - HALTZ_KEY_A];
    }
    if (usage >= 0x1E && usage < 0x1E + sizeof(row) / sizeof(row[0])) {
        return row[usage - 0x1E];
    }
    if (usage >= HALTZ_IO_USAGE_MOD_FIRST && usage < HALTZ_IO_USAGE_MOD_FIRST + 8) {
        return mods[usage - HALTZ_IO_USAGE_MOD_FIRST];
    }
    return "?";
}
