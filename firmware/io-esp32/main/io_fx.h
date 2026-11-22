/*
 * HALTZ I/O: buzzers and status LED (docs/io-protocol-v1.md sections 4, 5 and 8).
 * A task on core 1 advances the sequencer (tunes.c) every 1 ms and the LED pattern
 * (ledfx.c) every 10 ms, and drives the LEDC timers. The functions here may be called from
 * any task.
 */
#ifndef HALTZ_IO_FX_H
#define HALTZ_IO_FX_H

#include <stdint.h>

#include "esp_err.h"
#include "haltz/io_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LEDC, pins released, LED breathing blue, task started. Call once. */
esp_err_t fx_start(void);

/* An LED or sound command: IO_LED, IO_TONE, IO_NOTES, IO_SFX, IO_STOP, IO_VOLUME (already
 * unpacked, so the values are valid). HALTZ_ERR_NO_SPACE when the notes don't fit. */
haltz_err_t fx_cmd(const haltz_io_cmd_t *cmd);

/* For IO_STATUS: HALTZ_IO_STATUS_VOICE_A / _B bits, and free notes per voice. */
uint8_t fx_status(uint8_t tune_free[2]);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_FX_H */
