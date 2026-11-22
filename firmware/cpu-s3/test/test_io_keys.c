/* PC tests for io_keys.c: keyboard states -> key events and characters.
 * No framework: each CHECK prints on failure; exit code = fails. */
#include <stdio.h>
#include <string.h>

#include "io_keys.h"

static int g_checks;
static int g_fails;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        g_checks++;                                                                            \
        if (!(cond)) {                                                                         \
            g_fails++;                                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                      \
    } while (0)

typedef struct {
    int n;
    io_key_event_t ev[32];
} log_t;

static void collect(const io_key_event_t *ev, void *ctx) {
    log_t *l = ctx;
    if (l->n < 32) {
        l->ev[l->n] = *ev;
    }
    l->n++;
}

static haltz_io_keys_t keys(uint8_t mods, int n, const uint8_t *u) {
    haltz_io_keys_t k = {.modifiers = mods, .count = (uint8_t)n};
    memcpy(k.usages, u, (size_t)n);
    return k;
}

/* Feed a sequence of states, collect the typed characters. */
static void type_states(const haltz_io_keys_t *states, int n, bool *caps, char *out) {
    haltz_io_keys_t prev = {0};
    log_t l = {0};
    for (int i = 0; i < n; i++) {
        io_keys_diff(&prev, &states[i], caps, collect, &l);
        prev = states[i];
    }
    int j = 0;
    for (int i = 0; i < l.n && i < 32; i++) {
        if (l.ev[i].down && l.ev[i].ch) {
            out[j++] = l.ev[i].ch;
        }
    }
    out[j] = 0;
}

static void test_chars(void) {
    CHECK(io_key_char(HALTZ_KEY_A, 0, false) == 'a');
    CHECK(io_key_char(HALTZ_KEY_Z, HALTZ_IO_MOD_LSHIFT, false) == 'Z');
    CHECK(io_key_char(HALTZ_KEY_A, 0, true) == 'A');                       /* caps */
    CHECK(io_key_char(HALTZ_KEY_A, HALTZ_IO_MOD_RSHIFT, true) == 'a');     /* caps + shift */
    CHECK(io_key_char(0x06, HALTZ_IO_MOD_LCTRL, false) == 3);              /* Ctrl+C */
    CHECK(io_key_char(HALTZ_KEY_1, 0, false) == '1');
    CHECK(io_key_char(HALTZ_KEY_1, HALTZ_IO_MOD_LSHIFT, false) == '!');
    CHECK(io_key_char(HALTZ_KEY_0, HALTZ_IO_MOD_LSHIFT, false) == ')');
    CHECK(io_key_char(HALTZ_KEY_ENTER, 0, false) == '\n');
    CHECK(io_key_char(HALTZ_KEY_ESCAPE, 0, false) == 0x1B);
    CHECK(io_key_char(HALTZ_KEY_BACKSPACE, 0, false) == '\b');
    CHECK(io_key_char(HALTZ_KEY_TAB, 0, false) == '\t');
    CHECK(io_key_char(HALTZ_KEY_SPACE, HALTZ_IO_MOD_LSHIFT, false) == ' ');
    CHECK(io_key_char(0x2D, 0, false) == '-' && io_key_char(0x2D, HALTZ_IO_MOD_LSHIFT, false) == '_');
    CHECK(io_key_char(0x31, 0, false) == '\\' && io_key_char(0x31, HALTZ_IO_MOD_LSHIFT, false) == '|');
    CHECK(io_key_char(0x32, 0, false) == 0); /* non-US # */
    CHECK(io_key_char(0x34, HALTZ_IO_MOD_LSHIFT, false) == '"');
    CHECK(io_key_char(0x35, HALTZ_IO_MOD_LSHIFT, false) == '~');
    CHECK(io_key_char(0x38, 0, false) == '/' && io_key_char(0x38, HALTZ_IO_MOD_LSHIFT, false) == '?');
    CHECK(io_key_char(HALTZ_KEY_DELETE, 0, false) == 0x7F);
    CHECK(io_key_char(0x59, 0, false) == '1' && io_key_char(0x61, 0, false) == '9');
    CHECK(io_key_char(0x62, 0, false) == '0' && io_key_char(0x58, 0, false) == '\n');
    CHECK(io_key_char(HALTZ_KEY_F1, 0, false) == 0 && io_key_char(HALTZ_KEY_UP, 0, false) == 0);
    CHECK(io_key_char(HALTZ_KEY_CAPS_LOCK, 0, false) == 0);
    CHECK(strcmp(io_key_name(HALTZ_KEY_A), "A") == 0);
    CHECK(strcmp(io_key_name(HALTZ_KEY_ENTER), "Enter") == 0);
    CHECK(strcmp(io_key_name(HALTZ_KEY_F12), "F12") == 0);
    CHECK(strcmp(io_key_name(HALTZ_KEY_UP), "Up") == 0);
    CHECK(strcmp(io_key_name(HALTZ_KEY_PAGE_DOWN), "PgDn") == 0);
    CHECK(strcmp(io_key_name(0xE1), "LShift") == 0 && strcmp(io_key_name(0xE7), "RGUI") == 0);
    CHECK(strcmp(io_key_name(0x80), "?") == 0);
}

static void test_diff(void) {
    bool caps = false;
    log_t l = {0};
    haltz_io_keys_t none = {0};

    /* Shift and A in the same report: Shift down first, then A types 'A'. */
    const uint8_t a[] = {HALTZ_KEY_A};
    haltz_io_keys_t sa = keys(HALTZ_IO_MOD_LSHIFT, 1, a);
    io_keys_diff(&none, &sa, &caps, collect, &l);
    CHECK(l.n == 2);
    CHECK(l.ev[0].usage == 0xE1 && l.ev[0].down && l.ev[0].ch == 0);
    CHECK(l.ev[1].usage == HALTZ_KEY_A && l.ev[1].down && l.ev[1].ch == 'A');
    CHECK(l.ev[1].modifiers == HALTZ_IO_MOD_LSHIFT);

    /* Same state again (the refresh): nothing. */
    l.n = 0;
    io_keys_diff(&sa, &sa, &caps, collect, &l);
    CHECK(l.n == 0);

    /* Rollover: A released and B pressed in one report: A up before B down. */
    l.n = 0;
    const uint8_t b[] = {0x05};
    haltz_io_keys_t sb = keys(HALTZ_IO_MOD_LSHIFT, 1, b);
    io_keys_diff(&sa, &sb, &caps, collect, &l);
    CHECK(l.n == 2);
    CHECK(l.ev[0].usage == HALTZ_KEY_A && !l.ev[0].down && l.ev[0].ch == 0);
    CHECK(l.ev[1].usage == 0x05 && l.ev[1].down && l.ev[1].ch == 'B');

    /* Everything released (e.g. a link down): key up, then modifier up. */
    l.n = 0;
    io_keys_diff(&sb, &none, &caps, collect, &l);
    CHECK(l.n == 2);
    CHECK(l.ev[0].usage == 0x05 && !l.ev[0].down);
    CHECK(l.ev[1].usage == 0xE1 && !l.ev[1].down && l.ev[1].modifiers == 0);

    /* All 8 modifiers and 10 keys at once, and back. */
    l.n = 0;
    const uint8_t ten[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    haltz_io_keys_t full = keys(0xFF, 10, ten);
    io_keys_diff(&none, &full, &caps, collect, &l);
    CHECK(l.n == 18);
    CHECK(l.ev[0].usage == 0xE0 && l.ev[7].usage == 0xE7 && l.ev[8].usage == 4);
    CHECK(l.ev[8].ch == 1); /* Ctrl is held: Ctrl+A */
    l.n = 0;
    io_keys_diff(&full, &none, &caps, collect, &l);
    CHECK(l.n == 18 && !l.ev[0].down && l.ev[0].usage == 4 && l.ev[17].usage == 0xE7);

    /* Order inside the list doesn't matter. */
    l.n = 0;
    const uint8_t ab[] = {4, 5}, ba[] = {5, 4};
    haltz_io_keys_t k1 = keys(0, 2, ab), k2 = keys(0, 2, ba);
    io_keys_diff(&k1, &k2, &caps, collect, &l);
    CHECK(l.n == 0);
}

static void test_typing(void) {
    /* "Hi!" then Caps Lock, "ok", Caps Lock again, "x" */
    const uint8_t h[] = {0x0B}, i[] = {0x0C}, one[] = {HALTZ_KEY_1}, cl[] = {HALTZ_KEY_CAPS_LOCK},
                  o[] = {0x12}, k[] = {0x0E}, x[] = {0x1B};
    haltz_io_keys_t states[] = {
        keys(HALTZ_IO_MOD_LSHIFT, 0, NULL), keys(HALTZ_IO_MOD_LSHIFT, 1, h), keys(0, 0, NULL),
        keys(0, 1, i),  keys(0, 0, NULL),  keys(HALTZ_IO_MOD_RSHIFT, 1, one),
        keys(0, 0, NULL), keys(0, 1, cl),  keys(0, 0, NULL), keys(0, 1, o), keys(0, 0, NULL),
        keys(0, 1, k),  keys(0, 0, NULL),  keys(0, 1, cl),   keys(0, 0, NULL), keys(0, 1, x),
        keys(0, 0, NULL),
    };
    bool caps = false;
    char out[64];
    type_states(states, (int)(sizeof(states) / sizeof(states[0])), &caps, out);
    CHECK(strcmp(out, "Hi!OKx") == 0);
    CHECK(!caps);

    /* A quick tap of the same key twice (down, up, down, up) types it twice. */
    const uint8_t e[] = {0x08};
    haltz_io_keys_t tap[] = {keys(0, 1, e), keys(0, 0, NULL), keys(0, 1, e), keys(0, 0, NULL)};
    type_states(tap, 4, &caps, out);
    CHECK(strcmp(out, "ee") == 0);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"characters and names (US layout)", test_chars},
        {"state differences -> key events", test_diff},
        {"typing: shift, caps lock, taps", test_typing},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
