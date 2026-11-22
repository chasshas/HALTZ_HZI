/* HALTZ I/O: sound sequencer and the built-in sound effects. See tunes.h. */
#include "tunes.h"

#include <stddef.h>
#include <string.h>

/* ---- notes ------------------------------------------------------------------------------- */

/* MIDI notes 0-11 (C-1 .. B-1) in mHz; every octave up doubles. */
static const uint32_t k_octave_mhz[12] = {8176,  8662,  9177,  9723,  10301, 10913,
                                          11562, 12250, 12978, 13750, 14568, 15434};

uint16_t seq_midi_hz(uint8_t note) {
    if (note > 127) {
        note = 127;
    }
    uint32_t mhz = k_octave_mhz[note % 12] << (note / 12);
    return (uint16_t)((mhz + 500) / 1000);
}

uint16_t seq_gap_ms(uint16_t note_ms) {
    uint16_t gap = note_ms / 8;
    return gap > 20 ? 20 : gap;
}

/* ---- sound effects ----------------------------------------------------------------------- */
/* Square-wave sketches in {Hz, ms} steps (0 Hz = a pause); tune them by ear. Sweeps are
 * short steps. */

static const seq_step_t k_beep[] = {{1760, 90}};
static const seq_step_t k_click[] = {{3000, 6}};
static const seq_step_t k_key[] = {{4200, 2}};
static const seq_step_t k_ok[] = {{1319, 60}, {0, 15}, {1760, 110}};
static const seq_step_t k_error[] = {{220, 140}, {0, 40}, {165, 260}};
static const seq_step_t k_coin[] = {{988, 70}, {1319, 320}};
static const seq_step_t k_jump[] = {{392, 15}, {440, 15}, {494, 15}, {554, 15}, {622, 15},
                                    {698, 15}, {784, 15}, {880, 15}, {988, 15}, {1109, 20}};
static const seq_step_t k_power_up[] = {{523, 70}, {659, 70}, {784, 70}, {1047, 70}, {1319, 160}};
static const seq_step_t k_power_down[] = {{1047, 90}, {784, 90}, {659, 90}, {523, 90},
                                          {392, 110}, {262, 260}};
static const seq_step_t k_boot[] = {{784, 110}, {0, 20}, {1047, 110}, {0, 20},
                                    {1319, 110}, {0, 20}, {1568, 320}};
static const seq_step_t k_cart[] = {{2093, 40}, {0, 30}, {2637, 70}};
static const seq_step_t k_halt[] = {{880, 150}, {0, 50}, {440, 320}};
static const seq_step_t k_hit[] = {{1500, 10}, {700, 10},  {1200, 10}, {500, 10}, {900, 10},
                                   {350, 10},  {650, 12},  {250, 12},  {450, 14}, {180, 16},
                                   {300, 18},  {120, 24}};

#define SFX(name, steps) {name, steps, (uint8_t)(sizeof(steps) / sizeof(steps[0]))}
static const struct {
    const char *name;
    const seq_step_t *steps;
    uint8_t len;
} k_sfx[HALTZ_IO_SFX_COUNT] = {
    [HALTZ_IO_SFX_BEEP] = SFX("BEEP", k_beep),
    [HALTZ_IO_SFX_CLICK] = SFX("CLICK", k_click),
    [HALTZ_IO_SFX_KEY] = SFX("KEY", k_key),
    [HALTZ_IO_SFX_OK] = SFX("OK", k_ok),
    [HALTZ_IO_SFX_ERROR] = SFX("ERROR", k_error),
    [HALTZ_IO_SFX_COIN] = SFX("COIN", k_coin),
    [HALTZ_IO_SFX_JUMP] = SFX("JUMP", k_jump),
    [HALTZ_IO_SFX_POWER_UP] = SFX("POWER_UP", k_power_up),
    [HALTZ_IO_SFX_POWER_DOWN] = SFX("POWER_DOWN", k_power_down),
    [HALTZ_IO_SFX_BOOT] = SFX("BOOT", k_boot),
    [HALTZ_IO_SFX_CART] = SFX("CART", k_cart),
    [HALTZ_IO_SFX_HALT] = SFX("HALT", k_halt),
    [HALTZ_IO_SFX_HIT] = SFX("HIT", k_hit),
};
#undef SFX

const char *seq_sfx_name(uint8_t id) {
    return (id < HALTZ_IO_SFX_COUNT) ? k_sfx[id].name : "?";
}

uint32_t seq_sfx_ms(uint8_t id) {
    uint32_t ms = 0;
    if (id < HALTZ_IO_SFX_COUNT) {
        for (int i = 0; i < k_sfx[id].len; i++) {
            ms += k_sfx[id].steps[i].ms;
        }
    }
    return ms;
}

/* ---- voices ------------------------------------------------------------------------------ */

void seq_init(seq_t *s) {
    memset(s, 0, sizeof(*s));
}

static void clear_tune(seq_voice_t *v) {
    v->len = 0;
    v->pos = 0;
    v->elapsed = 0;
    v->loop = false;
}

void seq_stop(seq_t *s, int voice) {
    seq_voice_t *v = &s->v[voice];
    clear_tune(v);
    v->sfx = NULL;
}

void seq_tone(seq_t *s, int voice, uint16_t hz, uint16_t ms) {
    seq_voice_t *v = &s->v[voice];
    clear_tune(v);
    v->notes[0] = (seq_note_t){.hz = hz, .ms = ms, .legato = 1};
    v->len = 1;
}

haltz_err_t seq_notes(seq_t *s, const haltz_io_notes_t *m) {
    seq_voice_t *v = &s->v[m->voice];
    bool replace = (m->flags & HALTZ_IO_NOTES_REPLACE) != 0;
    uint16_t len = v->len, drop = 0;
    if (replace) {
        len = 0;
    } else if (!v->loop) {
        drop = v->pos; /* played notes leave the buffer */
        len = (uint16_t)(len - drop);
    }
    if ((unsigned)len + m->count > HALTZ_IO_TUNE_NOTES) {
        return HALTZ_ERR_NO_SPACE;
    }
    if (replace) {
        clear_tune(v);
    } else if (drop > 0) {
        memmove(&v->notes[0], &v->notes[drop], (size_t)len * sizeof(v->notes[0]));
        v->pos = (uint16_t)(v->pos - drop);
        v->len = len;
    }
    /* An endless tone can't be followed by anything: appending ends it. */
    if (!replace && v->pos < v->len && v->notes[v->pos].ms == 0) {
        clear_tune(v);
    }
    uint8_t legato = (m->flags & HALTZ_IO_NOTES_LEGATO) ? 1 : 0;
    for (int i = 0; i < m->count; i++) {
        uint8_t n = m->notes[i].note;
        v->notes[v->len++] = (seq_note_t){
            .hz = (n == HALTZ_IO_NOTE_REST) ? 0 : seq_midi_hz(n),
            .ms = (uint16_t)(m->notes[i].ticks * m->tick_ms),
            .legato = legato,
        };
    }
    if (m->flags & HALTZ_IO_NOTES_LOOP) {
        v->loop = true;
    }
    return HALTZ_OK;
}

void seq_sfx(seq_t *s, int voice, uint8_t id) {
    seq_voice_t *v = &s->v[voice];
    if (id >= HALTZ_IO_SFX_COUNT) {
        return;
    }
    v->sfx = k_sfx[id].steps;
    v->sfx_len = k_sfx[id].len;
    v->sfx_pos = 0;
    v->sfx_elapsed = 0;
}

static void advance_voice(seq_voice_t *v, uint32_t dt) {
    if (v->sfx != NULL) { /* the tune is paused meanwhile */
        uint32_t e = v->sfx_elapsed + dt;
        while (v->sfx_pos < v->sfx_len && e >= v->sfx[v->sfx_pos].ms) {
            e -= v->sfx[v->sfx_pos].ms;
            v->sfx_pos++;
        }
        if (v->sfx_pos >= v->sfx_len) {
            v->sfx = NULL; /* the rest of dt is dropped: the tune resumes exactly */
        } else {
            v->sfx_elapsed = (uint16_t)e;
        }
        return;
    }
    if (v->pos >= v->len) {
        return;
    }
    v->elapsed += dt;
    while (v->pos < v->len && v->notes[v->pos].ms != 0 && v->elapsed >= v->notes[v->pos].ms) {
        v->elapsed -= v->notes[v->pos].ms;
        v->pos++;
        if (v->pos >= v->len && v->loop) {
            v->pos = 0; /* every note is >= 1 ms long, so this ends */
        }
    }
    if (v->pos >= v->len) {
        v->elapsed = 0;
    }
}

void seq_advance(seq_t *s, uint32_t dt_ms) {
    for (int i = 0; i < HALTZ_IO_VOICES; i++) {
        advance_voice(&s->v[i], dt_ms);
    }
}

uint16_t seq_output_hz(const seq_t *s, int voice) {
    const seq_voice_t *v = &s->v[voice];
    if (v->sfx != NULL) {
        return v->sfx[v->sfx_pos].hz;
    }
    if (v->pos >= v->len) {
        return 0;
    }
    const seq_note_t *n = &v->notes[v->pos];
    if (n->hz == 0) {
        return 0;
    }
    if (n->ms != 0 && !n->legato && v->elapsed >= (uint32_t)(n->ms - seq_gap_ms(n->ms))) {
        return 0;
    }
    return n->hz;
}

uint8_t seq_free(const seq_t *s, int voice) {
    const seq_voice_t *v = &s->v[voice];
    unsigned used = v->loop ? v->len : (unsigned)(v->len - v->pos);
    return (uint8_t)(HALTZ_IO_TUNE_NOTES - used);
}
