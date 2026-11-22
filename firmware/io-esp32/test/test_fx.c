/* PC tests for the I/O board's sound sequencer (tunes.c) and LED patterns (ledfx.c).
 * No framework: each CHECK prints on failure; exit code = fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ledfx.h"
#include "tunes.h"

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

/* ---- sequencer --------------------------------------------------------------------------- */

static seq_t g_seq;
static uint32_t g_t; /* ms since the test started */

/* Advance in 1 ms steps (like io_fx.c) until time t. */
static void run_to(uint32_t t) {
    while (g_t < t) {
        seq_advance(&g_seq, 1);
        g_t++;
    }
}

static uint16_t hz_at(int voice, uint32_t t) {
    run_to(t);
    return seq_output_hz(&g_seq, voice);
}

static void reset_seq(void) {
    seq_init(&g_seq);
    g_t = 0;
}

static haltz_io_notes_t make_notes(uint8_t voice, uint8_t flags, uint8_t tick_ms,
                                   const uint8_t *pairs, int n) {
    haltz_io_notes_t m = {.voice = voice, .flags = flags, .tick_ms = tick_ms, .count = (uint8_t)n};
    for (int i = 0; i < n; i++) {
        m.notes[i] = (haltz_io_note_t){pairs[2 * i], pairs[2 * i + 1]};
    }
    return m;
}

static void test_midi(void) {
    CHECK(seq_midi_hz(69) == 440);
    CHECK(seq_midi_hz(81) == 880);
    CHECK(seq_midi_hz(60) == 262); /* C4 261.63 */
    CHECK(seq_midi_hz(12) == 16);  /* C0 16.35 */
    CHECK(seq_midi_hz(127) == 12544);
    CHECK(seq_midi_hz(72) == 523);
    for (int n = 13; n <= 127; n++) {
        CHECK(seq_midi_hz((uint8_t)n) > seq_midi_hz((uint8_t)(n - 1)));
    }
    CHECK(seq_gap_ms(100) == 12 && seq_gap_ms(1000) == 20 && seq_gap_ms(7) == 0);
}

static void test_tone(void) {
    reset_seq();
    CHECK(hz_at(0, 0) == 0 && seq_free(&g_seq, 0) == 255);
    seq_tone(&g_seq, 0, 1000, 100);
    CHECK(hz_at(0, 0) == 1000);
    CHECK(hz_at(0, 99) == 1000); /* a tone has no gap */
    CHECK(hz_at(0, 100) == 0);
    CHECK(hz_at(1, 100) == 0);
    seq_tone(&g_seq, 1, 300, 0); /* until stopped */
    CHECK(hz_at(1, 60000) == 300);
    seq_stop(&g_seq, 1);
    CHECK(hz_at(1, 60001) == 0);
}

static void test_notes_timing(void) {
    reset_seq();
    /* A4 100 ms, rest 25 ms, C5 50 ms (tick 25 ms) */
    const uint8_t t1[] = {69, 4, 0, 1, 72, 2};
    haltz_io_notes_t m = make_notes(0, HALTZ_IO_NOTES_REPLACE, 25, t1, 3);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    CHECK(seq_free(&g_seq, 0) == 252);
    CHECK(hz_at(0, 0) == 440);
    CHECK(hz_at(0, 87) == 440); /* gap = 100 / 8 = 12 ms */
    CHECK(hz_at(0, 88) == 0);
    CHECK(hz_at(0, 100) == 0); /* the rest */
    CHECK(hz_at(0, 124) == 0);
    CHECK(hz_at(0, 125) == 523);
    CHECK(hz_at(0, 168) == 523); /* 50 ms note: gap 6 */
    CHECK(hz_at(0, 169) == 0);
    CHECK(hz_at(0, 175) == 0); /* ended */
    CHECK(seq_free(&g_seq, 0) == 255);

    /* Legato: no gap. */
    reset_seq();
    m = make_notes(1, HALTZ_IO_NOTES_REPLACE | HALTZ_IO_NOTES_LEGATO, 25, t1, 1);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    CHECK(hz_at(1, 99) == 440 && hz_at(1, 100) == 0);

    /* Bigger steps than 1 ms give the same result. */
    reset_seq();
    m = make_notes(0, HALTZ_IO_NOTES_REPLACE, 25, t1, 3);
    seq_notes(&g_seq, &m);
    seq_advance(&g_seq, 130);
    CHECK(seq_output_hz(&g_seq, 0) == 523);
    seq_advance(&g_seq, 1000);
    CHECK(seq_output_hz(&g_seq, 0) == 0);
}

static void test_loop_and_append(void) {
    reset_seq();
    const uint8_t two[] = {60, 1, 64, 1};
    haltz_io_notes_t m = make_notes(0, HALTZ_IO_NOTES_REPLACE | HALTZ_IO_NOTES_LOOP, 100, two, 2);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    CHECK(hz_at(0, 50) == 262 && hz_at(0, 150) == 330);
    CHECK(hz_at(0, 250) == 262 && hz_at(0, 350) == 330); /* again */
    CHECK(hz_at(0, 100050) == 262);
    CHECK(seq_free(&g_seq, 0) == 253); /* looping notes stay in the buffer */
    /* Appending to a loop: the new note joins it. */
    const uint8_t one[] = {67, 1};
    m = make_notes(0, 0, 100, one, 1);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    CHECK(hz_at(0, 100250) == 392 && hz_at(0, 100350) == 262);

    /* Streaming without loop: played notes free their space. */
    reset_seq();
    uint8_t many[2 * HALTZ_IO_NOTES_MAX];
    for (int i = 0; i < HALTZ_IO_NOTES_MAX; i++) {
        many[2 * i] = (uint8_t)(48 + i % 24);
        many[2 * i + 1] = 1;
    }
    m = make_notes(1, HALTZ_IO_NOTES_REPLACE, 10, many, HALTZ_IO_NOTES_MAX);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    m = make_notes(1, 0, 10, many, HALTZ_IO_NOTES_MAX);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK); /* 252 */
    m = make_notes(1, 0, 10, many, 4);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_ERR_NO_SPACE); /* 256 > 255: none added */
    CHECK(seq_free(&g_seq, 1) == 3);
    run_to(25); /* 2 notes played, the third one sounding */
    CHECK(seq_free(&g_seq, 1) == 5);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    CHECK(seq_free(&g_seq, 1) == 1);
    CHECK(hz_at(1, 25) == seq_midi_hz(50)); /* still the third note after compacting */
    /* All 256 notes then play out: 2 + 250 + 4 = 256 notes of 10 ms. */
    CHECK(hz_at(1, 2555) != 0);
    CHECK(hz_at(1, 2560) == 0);

    /* A REPLACE starts over even if the buffer is full. */
    m = make_notes(1, HALTZ_IO_NOTES_REPLACE, 10, many, 1);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK && seq_free(&g_seq, 1) == 254);

    /* Appending after an endless tone ends the tone. */
    reset_seq();
    seq_tone(&g_seq, 0, 500, 0);
    m = make_notes(0, 0, 10, two, 2);
    CHECK(seq_notes(&g_seq, &m) == HALTZ_OK);
    CHECK(hz_at(0, 5) == 262 && hz_at(0, 15) == 330 && hz_at(0, 25) == 0);
}

static void test_sfx_over_tune(void) {
    reset_seq();
    /* One A4 of 1000 ms, then C5 of 100 ms. */
    const uint8_t t[] = {69, 10, 72, 1};
    haltz_io_notes_t m = make_notes(0, HALTZ_IO_NOTES_REPLACE, 100, t, 2);
    seq_notes(&g_seq, &m);
    CHECK(hz_at(0, 300) == 440);
    seq_sfx(&g_seq, 0, HALTZ_IO_SFX_BEEP); /* 1760 Hz, 90 ms */
    CHECK(hz_at(0, 300) == 1760);
    CHECK(hz_at(0, 389) == 1760);
    CHECK(hz_at(0, 390) == 440); /* the tune goes on where it was: 700 ms of A4 left */
    CHECK(hz_at(0, 390 + 679) == 440);
    CHECK(hz_at(0, 390 + 680) == 0);   /* its 20 ms gap */
    CHECK(hz_at(0, 390 + 700) == 523); /* next note on time, shifted by the effect */
    /* The other voice is not touched by an effect on this one. */
    CHECK(hz_at(1, 390 + 700) == 0);

    /* A new effect replaces one that is playing. */
    reset_seq();
    seq_sfx(&g_seq, 1, HALTZ_IO_SFX_ERROR);
    CHECK(hz_at(1, 10) == 220);
    seq_sfx(&g_seq, 1, HALTZ_IO_SFX_CLICK);
    CHECK(hz_at(1, 10) == 3000 && hz_at(1, 16) == 0);
    /* STOP ends an effect too. */
    seq_sfx(&g_seq, 1, HALTZ_IO_SFX_COIN);
    CHECK(hz_at(1, 20) != 0);
    seq_stop(&g_seq, 1);
    CHECK(hz_at(1, 21) == 0);

    /* Every effect: a name, 1 ms .. 2 s long, frequencies 0 or in the TONE range. */
    for (uint8_t id = 0; id < HALTZ_IO_SFX_COUNT; id++) {
        CHECK(strcmp(seq_sfx_name(id), "?") != 0);
        CHECK(seq_sfx_ms(id) >= 1 && seq_sfx_ms(id) <= 2000);
        reset_seq();
        seq_sfx(&g_seq, 0, id);
        CHECK(hz_at(0, 0) >= HALTZ_IO_TONE_HZ_MIN);
        for (uint32_t ms = 0; ms < seq_sfx_ms(id); ms++) {
            uint16_t hz = hz_at(0, ms);
            CHECK(hz == 0 || (hz >= HALTZ_IO_TONE_HZ_MIN && hz <= HALTZ_IO_TONE_HZ_MAX));
        }
        CHECK(hz_at(0, seq_sfx_ms(id)) == 0);
    }
    CHECK(strcmp(seq_sfx_name(HALTZ_IO_SFX_COUNT), "?") == 0);
}

/* ---- LED --------------------------------------------------------------------------------- */

static int same(rgb_t c, int r, int g, int b) {
    return c.r == r && c.g == g && c.b == b;
}

static int near(int a, int b, int tol) {
    return abs(a - b) <= tol;
}

static void led_set(ledfx_t *f, uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t p,
                    uint8_t count, uint32_t now) {
    haltz_io_led_t m = {mode, r, g, b, p, count};
    ledfx_set(f, &m, now);
}

static void test_led(void) {
    ledfx_t f;
    ledfx_init(&f);
    CHECK(same(ledfx_eval(&f, 0), 0, 0, 0));

    led_set(&f, HALTZ_IO_LED_SOLID, 255, 0, 10, 0, 0, 100);
    CHECK(same(ledfx_eval(&f, 100), 255, 0, 10) && same(ledfx_eval(&f, 99999), 255, 0, 10));

    led_set(&f, HALTZ_IO_LED_BLINK, 0, 0, 200, 400, 0, 1000);
    CHECK(same(ledfx_eval(&f, 1000), 0, 0, 200));
    CHECK(same(ledfx_eval(&f, 1199), 0, 0, 200));
    CHECK(same(ledfx_eval(&f, 1200), 0, 0, 0));
    CHECK(same(ledfx_eval(&f, 1399), 0, 0, 0));
    CHECK(same(ledfx_eval(&f, 1400), 0, 0, 200));

    /* FLASH over the blink, then the blink goes on in its own phase. */
    led_set(&f, HALTZ_IO_LED_FLASH, 255, 255, 255, 150, 0, 1250);
    CHECK(same(ledfx_eval(&f, 1250), 255, 255, 255));
    CHECK(same(ledfx_eval(&f, 1399), 255, 255, 255));
    CHECK(same(ledfx_eval(&f, 1400), 0, 0, 200));
    CHECK(same(ledfx_eval(&f, 1600), 0, 0, 0));
    /* A new pattern ends a flash that is still showing. */
    led_set(&f, HALTZ_IO_LED_FLASH, 255, 255, 255, 1000, 0, 2000);
    led_set(&f, HALTZ_IO_LED_SOLID, 0, 50, 0, 0, 0, 2010);
    CHECK(same(ledfx_eval(&f, 2011), 0, 50, 0));

    /* BREATHE: off at the start, full at half period, smooth and symmetric. */
    led_set(&f, HALTZ_IO_LED_BREATHE, 0, 255, 0, 1000, 0, 5000);
    CHECK(ledfx_eval(&f, 5000).g == 0);
    CHECK(near(ledfx_eval(&f, 5500).g, 255, 1));
    CHECK(near(ledfx_eval(&f, 5250).g, 128, 3));
    CHECK(near(ledfx_eval(&f, 5250).g, ledfx_eval(&f, 5750).g, 1));
    int last = -1, ok = 1;
    for (uint32_t t = 5000; t <= 5500; t += 5) {
        int g = ledfx_eval(&f, t).g;
        ok &= g >= last;
        last = g;
    }
    CHECK(ok);
    CHECK(ledfx_eval(&f, 6000).g == 0);

    /* FADE from what is showing (red) to blue. */
    led_set(&f, HALTZ_IO_LED_SOLID, 254, 0, 0, 0, 0, 7000);
    ledfx_eval(&f, 7000);
    led_set(&f, HALTZ_IO_LED_FADE, 0, 0, 254, 1000, 0, 7000);
    CHECK(same(ledfx_eval(&f, 7000), 254, 0, 0));
    rgb_t mid = ledfx_eval(&f, 7500);
    CHECK(near(mid.r, 127, 1) && mid.g == 0 && near(mid.b, 127, 1));
    CHECK(same(ledfx_eval(&f, 8000), 0, 0, 254) && same(ledfx_eval(&f, 9000), 0, 0, 254));
    led_set(&f, HALTZ_IO_LED_FADE, 9, 9, 9, 0, 0, 9000); /* period 0: at once */
    CHECK(same(ledfx_eval(&f, 9000), 9, 9, 9));

    /* CODE 2: blink, blink, pause of 3 periods. */
    led_set(&f, HALTZ_IO_LED_CODE, 255, 0, 0, 300, 2, 10000);
    CHECK(ledfx_eval(&f, 10000).r == 255 && ledfx_eval(&f, 10150).r == 0);
    CHECK(ledfx_eval(&f, 10300).r == 255 && ledfx_eval(&f, 10450).r == 0);
    CHECK(ledfx_eval(&f, 10600).r == 0 && ledfx_eval(&f, 11499).r == 0);
    CHECK(ledfx_eval(&f, 11500).r == 255);

    /* Time wrapping around 2^32 ms (49 days) doesn't break a flash. */
    led_set(&f, HALTZ_IO_LED_SOLID, 1, 2, 3, 0, 0, 0xFFFFFF00u);
    led_set(&f, HALTZ_IO_LED_FLASH, 255, 255, 255, 0x200, 0, 0xFFFFFF00u);
    CHECK(same(ledfx_eval(&f, 0x00000010u), 255, 255, 255));
    CHECK(same(ledfx_eval(&f, 0x00000100u), 1, 2, 3));
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"MIDI note frequencies", test_midi},
        {"TONE", test_tone},
        {"NOTES timing, gaps, legato", test_notes_timing},
        {"loop, append, streaming, NO_SPACE", test_loop_and_append},
        {"sound effects over a tune", test_sfx_over_tune},
        {"LED patterns", test_led},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i].fn();
        printf("%s %s\n", g_fails == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
