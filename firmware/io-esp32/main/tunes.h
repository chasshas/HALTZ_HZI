/*
 * HALTZ I/O: the two-voice sound sequencer (docs/io-protocol-v1.md section 4).
 * Portable C (no ESP-IDF calls), so the PC tests in firmware/io-esp32/test can drive it.
 *
 * The sequencer only decides which frequency each voice plays at each moment. io_fx.c calls
 * seq_advance() every millisecond and puts seq_output_hz() on the buzzer timers.
 *
 * Each voice has a tune buffer of HALTZ_IO_TUNE_NOTES notes and an optional sound effect on
 * top: while the effect plays the tune is paused, then it continues where it was.
 * Not thread safe: the caller locks (io_fx.c).
 */
#ifndef HALTZ_IO_TUNES_H
#define HALTZ_IO_TUNES_H

#include <stdbool.h>
#include <stdint.h>

#include "haltz/io.h"
#include "haltz/io_msgs.h"
#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t hz;    /* 0 = rest */
    uint16_t ms;    /* 0 = until stopped (IO_TONE only) */
    uint8_t legato; /* 1 = no gap at the end */
} seq_note_t;

typedef struct {
    uint16_t hz;
    uint16_t ms;
} seq_step_t;

typedef struct {
    seq_note_t notes[HALTZ_IO_TUNE_NOTES];
    uint16_t len;     /* notes in the buffer */
    uint16_t pos;     /* the note playing; len = the tune has ended */
    uint32_t elapsed; /* ms into notes[pos] */
    bool loop;
    /* Sound effect on top (NULL = none) */
    const seq_step_t *sfx;
    uint8_t sfx_len, sfx_pos;
    uint16_t sfx_elapsed;
} seq_voice_t;

typedef struct {
    seq_voice_t v[HALTZ_IO_VOICES];
} seq_t;

void seq_init(seq_t *s);

/* IO_STOP for one voice: silence, empty buffer, no effect. */
void seq_stop(seq_t *s, int voice);
/* IO_TONE: replaces the voice's tune with one frequency (ms 0 = until stopped). */
void seq_tone(seq_t *s, int voice, uint16_t hz, uint16_t ms);
/* IO_NOTES (already value-checked by the unpack). HALTZ_ERR_NO_SPACE: nothing added. */
haltz_err_t seq_notes(seq_t *s, const haltz_io_notes_t *m);
/* IO_SFX (id < HALTZ_IO_SFX_COUNT). */
void seq_sfx(seq_t *s, int voice, uint8_t id);

/* Move time forward. */
void seq_advance(seq_t *s, uint32_t dt_ms);
/* Frequency the voice should play now, 0 = silent (pin released). */
uint16_t seq_output_hz(const seq_t *s, int voice);
/* Free notes in the tune buffer (IO_STATUS tune_free). */
uint8_t seq_free(const seq_t *s, int voice);

/* MIDI note number (0..127) to Hz, rounded. 69 = 440. */
uint16_t seq_midi_hz(uint8_t note);
/* The gap at the end of a non-legato note: 1/8 of it, at most 20 ms. */
uint16_t seq_gap_ms(uint16_t note_ms);

const char *seq_sfx_name(uint8_t id);
/* Total length of an effect in ms (for tests and the console). */
uint32_t seq_sfx_ms(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_TUNES_H */
