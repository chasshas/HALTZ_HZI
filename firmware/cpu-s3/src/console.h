/*
 * HALTZ CPU: the text console on the TFT (architecture-v1 bring-up step 4).
 *
 * Everything the firmware prints (printf, ESP_LOG) still goes to the serial ports and now
 * also appears in a window of the GPU's text layer:
 *   - con_early_init() replaces stdout with a tee (a VFS device, /dev/hzcon) that writes to
 *     the normal console and into a 4 KB ring buffer. Writing never blocks and never calls
 *     the GPU driver, so printf stays safe everywhere (also inside gpu.c's locked code).
 *     When the ring is full, bytes are dropped for the TFT only (con_dropped()).
 *   - con_start() starts the console task (core 0): it feeds the ring into the terminal
 *     engine (con_term.c), which sends GPU text commands, then calls gpu_present(). While a
 *     frame is on its way, more text collects, so a burst of log lines costs few frames.
 * Only the console task touches the engine; the other calls are requests to it.
 */
#ifndef HALTZ_CPU_CONSOLE_H
#define HALTZ_CPU_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in app_main: stdout -> serial + ring. Output from then on is kept (up to 4 KB)
 * until con_start(). Tasks created later inherit the new stdout. */
esp_err_t con_early_init(void);

/* Start the console task with a window of rows top .. top + height - 1 (cleared first).
 * gpu_start() must have been called (the link may still be coming up: drawing goes into
 * the driver's shadow and is shown at link-up). */
esp_err_t con_start(uint8_t top, uint8_t height);

/* Bytes for the TFT only (the stdout tee calls this; usable directly too). Never blocks. */
void con_write(const void *data, size_t n);

/* Move the window. Rows that leave it are not cleared (draw over them). */
void con_set_window(uint8_t top, uint8_t height);
/* Draw the window again (after a GPU_INIT, or anything drawn over it). */
void con_redraw(void);
/* hold = true: keep recording output but stop drawing (someone else owns the screen);
 * hold = false: draw the window again and carry on. */
void con_hold(bool hold);

/* Bytes dropped because the ring was full (TFT only; the serial ports got them). */
uint32_t con_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_CONSOLE_H */
