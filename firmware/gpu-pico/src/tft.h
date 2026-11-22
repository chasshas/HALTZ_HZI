/*
 * ST7789V panel on SPI1 (master), landscape 320 x 240. Used by core 1 only, except
 * tft_backlight(), which only touches the PWM and may be called from core 0.
 *
 * Pixels go out as 16-bit SPI frames, so RGB565 values are sent high byte first without
 * swapping. One rectangle at a time:
 *     tft_begin_rect(x, y, w, h);
 *     tft_send(buf_a, n); tft_send(buf_b, n); ...   (DMA; returns as soon as it started)
 *     tft_end_rect();
 * A buffer passed to tft_send must not change until the next tft_send or tft_end_rect
 * returns (that is when its DMA is finished).
 */
#ifndef HALTZ_GPU_TFT_H
#define HALTZ_GPU_TFT_H

#include <stddef.h>
#include <stdint.h>

/* ---- panel settings (found with the wiring test) ---- */
#define TFT_SPI_HZ  40000000 /* gives 37.5 MHz (150 MHz / 4). Garbled picture: try 25000000 */
#define TFT_MADCTL  0xA0     /* MY | MV: landscape, turned 180 degrees (as mounted) */
#define TFT_INVERT  1        /* INVON for this IPS panel */
#define TFT_BGR     0        /* 1 if red and blue are swapped */

void tft_init(void);              /* reset + init sequence, clears to black, backlight full */
void tft_backlight(uint8_t level); /* 0 off .. 255 full */
unsigned tft_spi_hz(void);         /* the clock actually used */

void tft_begin_rect(int x, int y, int w, int h);
void tft_send(const uint16_t *pixels, size_t count);
void tft_end_rect(void);

#endif /* HALTZ_GPU_TFT_H */
