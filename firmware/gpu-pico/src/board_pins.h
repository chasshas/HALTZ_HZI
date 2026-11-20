/*
 * HALTZ GPU board (Raspberry Pi Pico 2 W #1, RP2350): pin assignments.
 * Constants only. These pins are fixed by the wiring; don't change them here alone.
 */
#ifndef HALTZ_GPU_BOARD_PINS_H
#define HALTZ_GPU_BOARD_PINS_H

/* ---- ST7789V 240x320 TFT on SPI1 (master) ---- */
#define PIN_TFT_SCK  10 /* SPI1 SCK */
#define PIN_TFT_MOSI 11 /* SPI1 TX */
#define PIN_TFT_CS   13 /* chip select, active low (SPI1 CSn) */
#define PIN_TFT_DC   14 /* data/command select */
#define PIN_TFT_RST  15 /* panel reset, active low */
#define PIN_TFT_BL   22 /* backlight, PWM (slice 3 channel A) */

/* ---- Link from the S3 on SPI0 (slave) ---- */
#define PIN_LINK_RX    16 /* SPI0 RX: data from the S3 (its MOSI) */
#define PIN_LINK_CSN   17 /* SPI0 CSn: chip select from the S3 */
#define PIN_LINK_SCK   18 /* SPI0 SCK: clock from the S3 */
#define PIN_LINK_TX    19 /* SPI0 TX: data to the S3 (its MISO) */
#define PIN_LINK_BUSY  20 /* out: high while the GPU can't take commands */
#define PIN_LINK_FRAME 21 /* out: frame/vsync signal to the S3 */

/* ---- stdio on UART0 to the debug probe ---- */
#define PIN_STDIO_UART_TX 0 /* UART0 TX -> probe RX */
#define PIN_STDIO_UART_RX 1 /* UART0 RX <- probe TX */

/*
 * Not on the header / never use (Pico 2 W wireless chip and board functions):
 *   GP23, GP24, GP25, GP29
 */

#endif /* HALTZ_GPU_BOARD_PINS_H */
