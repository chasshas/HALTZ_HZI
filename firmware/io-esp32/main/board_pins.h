/*
 * HALTZ I/O board (ESP32 DevKit V1, ESP32-WROOM-32, 30-pin): pin assignments.
 * Constants only. These pins are fixed by the wiring; don't change them here alone.
 */
#ifndef HALTZ_IO_BOARD_PINS_H
#define HALTZ_IO_BOARD_PINS_H

/* ---- UART2 link to the S3 ---- */
#define PIN_CPU_UART_RX 16 /* RX2, from S3 UART1 TX (GPIO17) */
#define PIN_CPU_UART_TX 17 /* TX2, to S3 UART1 RX (GPIO18) */

/* ---- Thumbstick ---- */
#define PIN_STICK_VRX 34 /* ADC1_CH6, input only */
#define PIN_STICK_VRY 35 /* ADC1_CH7, input only */
#define PIN_STICK_SW  32 /* push switch to GND, use the internal pull-up, active low */

/* ---- Knob (10 k pot wiper) ---- */
#define PIN_KNOB 36 /* ADC1_CH0 (SENSOR_VP), input only */

/* ---- RGB LED module (resistors on board, common cathode: HIGH = on) ---- */
#define PIN_LED_R 18
#define PIN_LED_G 19
#define PIN_LED_B 21

/* ---- Buzzers (3-pin modules GND/VCC/SIG, 1 k resistor in each SIG line) ---- */
/* Silent = pin set to input with no pull (not driven HIGH or LOW). */
/* Playing = 50% square wave. GPIO25/26 are also the DAC pins. */
#define PIN_BUZZER_A 25 /* voice A */
#define PIN_BUZZER_B 26 /* voice B */

/* ---- Spare, free for later ---- */
/* 4, 13, 14, 22, 23, 27, 33, 39 (39 is input only) */

/*
 * Never use:
 *   6-11            on-module SPI flash
 *   0, 2, 5, 12, 15 strapping pins
 *   1, 3            UART0 (USB console)
 * GPIO34-39 are input-only and have no internal pull-ups.
 */

#endif /* HALTZ_IO_BOARD_PINS_H */
