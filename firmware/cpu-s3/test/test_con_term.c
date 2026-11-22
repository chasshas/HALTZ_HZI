/*
 * PC test for con_term.c (the console's terminal engine).
 *
 * The engine's commands go through the real wire path (pack, frame encode, decode,
 * haltz_gpu_cmd_unpack) into a gpu_state_t, the GPU's own picture state. Fixed cases check
 * what ends up where; random byte streams check that the screen always equals the engine's
 * own cells, that rows outside the window are never touched, and that a redraw into a fresh
 * GPU_INIT state gives the same window.
 */
#include <stdio.h>
#include <string.h>

#include "con_term.h"
#include "haltz/packet.h"

#include "../../gpu-pico/src/gpu_state.h"

static int g_checks, g_fails;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        g_checks++;                                                                            \
        if (!(cond)) {                                                                         \
            g_fails++;                                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                      \
    } while (0)

/* ---- the screen: commands through the wire into a gpu_state_t ---------------------------- */

typedef struct {
    gpu_state_t *s;
    haltz_decoder_t dec;
    int bad;
    size_t bytes;
} screen_t;

static void to_screen(const haltz_gpu_cmd_t *cmd, void *ctx) {
    screen_t *sc = ctx;
    uint8_t payload[HALTZ_MAX_PAYLOAD], frame[HALTZ_MAX_FRAME];
    size_t n = 0, fn = 0;
    if (haltz_gpu_cmd_pack(cmd, payload, sizeof(payload), &n) != HALTZ_OK ||
        haltz_frame_encode(cmd->type, payload, n, frame, sizeof(frame), &fn) != HALTZ_OK) {
        sc->bad++;
        return;
    }
    sc->bytes += fn;
    for (size_t i = 0; i < fn; i++) {
        for (haltz_dec_result_t r = haltz_decoder_feed(&sc->dec, frame[i]);
             r != HALTZ_DEC_NEED_MORE; r = haltz_decoder_poll(&sc->dec)) {
            static haltz_gpu_cmd_t got;
            const haltz_frame_t *f = haltz_decoder_frame(&sc->dec);
            if (r != HALTZ_DEC_FRAME ||
                haltz_gpu_cmd_unpack(&got, f->type, f->payload, f->len) != HALTZ_OK) {
                sc->bad++;
                continue;
            }
            gpu_state_apply(sc->s, &got);
        }
    }
}

static gpu_state_t g_state, g_fresh;
static screen_t g_screen;
static con_term_t g_term;

static void setup(uint8_t top, uint8_t height) {
    gpu_state_init(&g_state);
    memset(&g_screen, 0, sizeof(g_screen));
    g_screen.s = &g_state;
    haltz_decoder_init(&g_screen.dec);
    con_term_init(&g_term, top, height, to_screen, &g_screen);
}

static void feed(const char *s) {
    con_term_feed(&g_term, (const uint8_t *)s, strlen(s));
    con_term_flush(&g_term);
}

/* Screen row as a string with trailing spaces cut. */
static const char *row_text(const gpu_state_t *s, int r) {
    static char buf[CON_COLS + 1];
    memcpy(buf, s->text_ch[r], CON_COLS);
    int n = CON_COLS;
    while (n > 0 && buf[n - 1] == ' ') {
        n--;
    }
    buf[n] = 0;
    return buf;
}

/* Window rows of the screen equal the engine's cells. */
static int window_matches(const gpu_state_t *s, const con_term_t *t) {
    for (int r = t->top; r < t->top + t->height; r++) {
        if (memcmp(s->text_ch[r], t->ch[r], CON_COLS) != 0 ||
            memcmp(s->text_fg[r], t->cfg[r], CON_COLS) != 0 ||
            memcmp(s->text_bg[r], t->cbg[r], CON_COLS) != 0) {
            return 0;
        }
    }
    return 1;
}

static int cursor_matches(const gpu_state_t *s, const con_term_t *t) {
    int c = t->col >= CON_COLS ? CON_COLS - 1 : t->col;
    return s->cursor_col == c && s->cursor_row == t->row &&
           s->cursor_style == (HALTZ_GPU_CURSOR_UNDERLINE | HALTZ_GPU_CURSOR_BLINK);
}

/* ---- fixed cases ------------------------------------------------------------------------- */

static void test_basic_text(void) {
    setup(0, CON_ROWS);
    feed("hello\nworld");
    CHECK(strcmp(row_text(&g_state, 0), "hello") == 0);
    CHECK(strcmp(row_text(&g_state, 1), "world") == 0);
    CHECK(g_term.col == 5 && g_term.row == 1 && cursor_matches(&g_state, &g_term));
    feed("\rW\tx\b\bY");
    CHECK(strcmp(row_text(&g_state, 1), "World  Yx") == 0); /* tab to 8, x at 8, back 2: Y at 7 */
    CHECK(g_screen.bad == 0);
}

static void test_wrap(void) {
    setup(0, CON_ROWS);
    char line[CON_COLS + 1];
    memset(line, 'a', CON_COLS);
    line[CON_COLS] = 0;
    feed(line);
    feed("\nb"); /* exactly 40 then \n: one row, no blank row */
    CHECK(strcmp(row_text(&g_state, 0), line) == 0);
    CHECK(strcmp(row_text(&g_state, 1), "b") == 0);
    feed("\n");
    feed(line);
    feed("c"); /* the 41st character goes to the next row */
    CHECK(strcmp(row_text(&g_state, 2), line) == 0);
    CHECK(strcmp(row_text(&g_state, 3), "c") == 0);
    CHECK(window_matches(&g_state, &g_term));
}

static void test_scroll_window(void) {
    setup(5, 3);
    /* Something outside the window: must survive. */
    haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TEXT_FILL};
    c.u.text_fill = (haltz_gpu_text_fill_t){0, 0, CON_COLS, CON_ROWS, 'X', 3, 4};
    gpu_state_apply(&g_state, &c);
    con_term_clear(&g_term);
    feed("1\n2\n3\n4\n5");
    CHECK(strcmp(row_text(&g_state, 5), "3") == 0);
    CHECK(strcmp(row_text(&g_state, 6), "4") == 0);
    CHECK(strcmp(row_text(&g_state, 7), "5") == 0);
    CHECK(g_state.text_ch[4][0] == 'X' && g_state.text_ch[8][0] == 'X');
    CHECK(g_state.text_fg[4][39] == 3 && g_state.text_bg[8][39] == 4);
    CHECK(window_matches(&g_state, &g_term) && cursor_matches(&g_state, &g_term));

    /* A one-row window: every newline clears it. */
    setup(29, 1);
    feed("first\nsecond");
    CHECK(strcmp(row_text(&g_state, 29), "second") == 0);
    CHECK(window_matches(&g_state, &g_term));
}

static void test_colours_and_escapes(void) {
    setup(0, CON_ROWS);
    feed("\x1b[31mR\x1b[0mW\x1b[1;32mG\x1b[mD\x1b[97;44mB\x1b[39;49mN");
    CHECK(strcmp(row_text(&g_state, 0), "RWGDBN") == 0);
    CHECK(g_state.text_fg[0][0] == HALTZ_COLOR_RED);
    CHECK(g_state.text_fg[0][1] == CON_FG_DEFAULT);
    CHECK(g_state.text_fg[0][2] == HALTZ_COLOR_GREEN); /* bold ignored */
    CHECK(g_state.text_fg[0][3] == CON_FG_DEFAULT);
    CHECK(g_state.text_fg[0][4] == HALTZ_COLOR_WHITE && g_state.text_bg[0][4] == HALTZ_COLOR_BLUE);
    CHECK(g_state.text_fg[0][5] == CON_FG_DEFAULT && g_state.text_bg[0][5] == CON_BG_DEFAULT);

    /* Other sequences print nothing; a control byte inside one still acts. */
    setup(0, CON_ROWS);
    feed("a\x1b[2Jb\x1b[?25lc\x1b" "Zd\x1b[12;34Hz\x1b[3\nq");
    CHECK(strcmp(row_text(&g_state, 0), "abcdz") == 0);
    CHECK(strcmp(row_text(&g_state, 1), "q") == 0);
    CHECK(window_matches(&g_state, &g_term));
}

static void test_redraw(void) {
    setup(5, 24);
    feed("one\n\x1b[33mtwo\x1b[0m\nthree");
    gpu_state_init(&g_fresh); /* as after a GPU_INIT */
    g_screen.s = &g_fresh;
    con_term_redraw(&g_term);
    CHECK(window_matches(&g_fresh, &g_term) && cursor_matches(&g_fresh, &g_term));
    CHECK(strcmp(row_text(&g_fresh, 6), "two") == 0 && g_fresh.text_fg[6][0] == HALTZ_COLOR_YELLOW);
}

/* The boot case: a full-screen log, then the window shrinks under the cursor. */
static void test_shrink_window(void) {
    setup(0, CON_ROWS);
    char line[16];
    for (int i = 0; i < 35; i++) {
        snprintf(line, sizeof(line), "L%d\n", i);
        feed(line);
    }
    feed("tail");
    CHECK(g_term.row == 29 && strcmp(row_text(&g_state, 29), "tail") == 0);
    con_term_set_window(&g_term, 5, 24);
    con_term_redraw(&g_term);
    CHECK(strcmp(row_text(&g_state, 28), "tail") == 0);
    CHECK(strcmp(row_text(&g_state, 27), "L34") == 0);
    CHECK(strcmp(row_text(&g_state, 5), "L12") == 0);
    feed("X\nnext");
    CHECK(strcmp(row_text(&g_state, 27), "tailX") == 0);
    CHECK(strcmp(row_text(&g_state, 28), "next") == 0);
    CHECK(window_matches(&g_state, &g_term) && cursor_matches(&g_state, &g_term));

    /* Cursor inside the new window: nothing moves. */
    setup(0, CON_ROWS);
    feed("a\nb\nc");
    con_term_set_window(&g_term, 1, 10);
    con_term_redraw(&g_term);
    CHECK(strcmp(row_text(&g_state, 1), "b") == 0 && strcmp(row_text(&g_state, 2), "c") == 0);
    CHECK(g_term.row == 2 && window_matches(&g_state, &g_term));
}

/* ---- random streams ---------------------------------------------------------------------- */

static uint32_t g_rng = 2026;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static size_t random_chunk(uint8_t *buf, size_t cap) {
    size_t n = 0;
    while (n + 16 < cap && (rnd() % 64) != 0) {
        uint32_t k = rnd() % 100;
        if (k < 60) {
            buf[n++] = (uint8_t)(0x20 + rnd() % 95);
        } else if (k < 70) {
            buf[n++] = '\n';
        } else if (k < 73) {
            buf[n++] = (uint8_t)"\r\b\t"[rnd() % 3];
        } else if (k < 78) {
            buf[n++] = (uint8_t)(0x80 + rnd() % 128);
        } else if (k < 88) { /* a colour */
            static const char *const sgr[] = {"\x1b[0m", "\x1b[31m", "\x1b[92m", "\x1b[44m",
                                              "\x1b[1;33;41m", "\x1b[m", "\x1b[39m", "\x1b[107m"};
            const char *s = sgr[rnd() % 8];
            size_t l = strlen(s);
            memcpy(buf + n, s, l);
            n += l;
        } else if (k < 93) { /* junk escapes, cut anywhere */
            static const char *const junk[] = {"\x1b[2J", "\x1b[?25h", ("\x1b" "c"), "\x1b[",
                                               "\x1b[12;", "\x1b\x1b[K"};
            const char *s = junk[rnd() % 6];
            size_t l = strlen(s);
            memcpy(buf + n, s, l);
            n += l;
        } else {
            buf[n++] = (uint8_t)(rnd() % 32); /* any control byte */
        }
    }
    return n;
}

static void test_random_streams(void) {
    static const uint8_t windows[][2] = {{0, CON_ROWS}, {5, 24}, {29, 1}, {0, 2}};
    int bad = 0;
    for (int w = 0; w < 4; w++) {
        for (int run = 0; run < 100; run++) {
            setup(windows[w][0], windows[w][1]);
            /* Mark the rows outside the window. */
            haltz_gpu_cmd_t c = {.type = HALTZ_MSG_GPU_TEXT_FILL};
            c.u.text_fill = (haltz_gpu_text_fill_t){0, 0, CON_COLS, CON_ROWS, 'X', 3, 4};
            gpu_state_apply(&g_state, &c);
            con_term_clear(&g_term);
            int ok = 1;
            for (int chunk = 0; chunk < 40 && ok; chunk++) {
                uint8_t buf[512];
                size_t n = random_chunk(buf, sizeof(buf));
                con_term_feed(&g_term, buf, n);
                con_term_flush(&g_term);
                ok = window_matches(&g_state, &g_term) && cursor_matches(&g_state, &g_term);
            }
            for (int r = 0; r < CON_ROWS && ok; r++) {
                if (r < g_term.top || r >= g_term.top + g_term.height) {
                    for (int col = 0; col < CON_COLS; col++) {
                        ok &= g_state.text_ch[r][col] == 'X' && g_state.text_fg[r][col] == 3;
                    }
                }
            }
            gpu_state_init(&g_fresh);
            g_screen.s = &g_fresh;
            con_term_redraw(&g_term);
            ok &= window_matches(&g_fresh, &g_term) && cursor_matches(&g_fresh, &g_term);
            ok &= g_screen.bad == 0;
            bad += !ok;
        }
    }
    CHECK(bad == 0);
    printf("  400 random streams in 4 windows, %d wrong\n", bad);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"text, \\r \\t \\b", test_basic_text},
        {"wrap at column 40", test_wrap},
        {"scrolling inside the window", test_scroll_window},
        {"colours and escape sequences", test_colours_and_escapes},
        {"redraw after GPU_INIT", test_redraw},
        {"window shrinks under the cursor", test_shrink_window},
        {"random streams match the screen", test_random_streams},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
