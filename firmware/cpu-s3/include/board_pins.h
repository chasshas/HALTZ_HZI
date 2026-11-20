/*
 * HALTZ CPU board (ESP32-S3-DevKitC-1, WROOM-1 N16R8): pin assignments.
 * Constants only. These pins are fixed by the wiring; don't change them here alone.
 */
#ifndef HALTZ_CPU_BOARD_PINS_H
#define HALTZ_CPU_BOARD_PINS_H

/* ---- RC522 card reader on SPI host B (SPI3_HOST, GPIO matrix) ---- */
#define PIN_RC522_SCK  4  /* SPI clock out */
#define PIN_RC522_MOSI 5  /* data to RC522 */
#define PIN_RC522_MISO 6  /* data from RC522 */
#define PIN_RC522_CS   7  /* chip select, active low */
#define PIN_RC522_RST  15 /* RC522 reset (NRSTPD), active low */

/* ---- GPU link to Pico #1 on SPI host A (SPI2_HOST / FSPI, master) ---- */
/* SCK/MOSI/MISO/CS are the FSPI IO_MUX pins, so the link can run at full speed. */
#define PIN_GPU_SCK      12 /* SPI clock out */
#define PIN_GPU_MOSI     11 /* commands to the Pico */
#define PIN_GPU_MISO     13 /* replies from the Pico */
#define PIN_GPU_CS       10 /* chip select, active low */
#define PIN_GPU_BUSY     9  /* in: Pico is busy, don't send */
#define PIN_GPU_FRAME    14 /* in: Pico frame/vsync signal */
#define PIN_GPU_PICO_RUN 21 /* out, open-drain: pull low to hold the Pico in reset */

/* ---- I/O link to the ESP32 on UART1 ---- */
#define PIN_IO_UART_TX 17 /* to ESP32 RX2 (GPIO16) */
#define PIN_IO_UART_RX 18 /* from ESP32 TX2 (GPIO17) */
#define PIN_IO_EN      8  /* out, open-drain: pull low to reset the ESP32 (its EN pin) */

/* ---- Buttons ---- */
#define PIN_POWER_BUTTON 16 /* pull-up, active low; RTC GPIO, wakes from deep sleep */
#define PIN_HALT_BUTTON  0  /* on-board BOOT button = HALT. Strapping pin: read only after boot */

/*
 * Reserved, never use:
 *   0          strapping (only read as HALT after boot, see above)
 *   3          strapping (JTAG source select)
 *   19, 20     native USB D-/D+
 *   35, 36, 37 octal PSRAM bus (N16R8 module)
 *   43, 44     UART0 TX/RX (console via the DevKit's UART USB port)
 *   45, 46     strapping (VDD_SPI voltage, boot mode / ROM log)
 *   38 or 48   on-board RGB LED, depending on DevKitC-1 board revision
 *
 * Spare, free for later: 1, 2, 47, 39-42 (39-42 are JTAG, free while debugging over USB).
 * UART2 is free.
 */

#endif /* HALTZ_CPU_BOARD_PINS_H */
