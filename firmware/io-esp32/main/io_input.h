/*
 * HALTZ I/O: thumbstick, knob and buttons (docs/io-protocol-v1.md section 3).
 * ADC1 oneshot, full range (12 dB), 12 bit. Called from the io_link task only.
 */
#ifndef HALTZ_IO_INPUT_H
#define HALTZ_IO_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "haltz/io_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    haltz_io_stick_t stick; /* as last reported */
    uint8_t knob;           /* as last reported */
    bool stick_ok;          /* centre plausible (otherwise the axes read 0) */
    /* Raw, for the console */
    int raw_x, raw_y, raw_knob;
    int centre_x, centre_y;
} input_state_t;

/* ADC and button pins, then input_calibrate(). */
esp_err_t input_start(void);
/* Measure the stick centre (the stick must be released). About 15 ms. */
void input_calibrate(void);
/* One 10 ms sample. Sets *stick_changed / *knob_changed when the reported value changed
 * enough to be sent (protocol section 3); the state is then updated. */
void input_sample(bool *stick_changed, bool *knob_changed);
const input_state_t *input_state(void);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_INPUT_H */
