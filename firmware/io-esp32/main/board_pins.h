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

/* ---- RGB LED ---- */
#define PIN_LED_R 18
#define PIN_LED_G 19
#define PIN_LED_B 21

/* ---- Buzzers ---- */
/* Silent = pin set to input with no pull (not driven HIGH or LOW). */
#define PIN_BUZZER_A 25 /* voice A */
#define PIN_BUZZER_B 26 /* voice B */

/* ---- Spare, free for later ---- */
/* 4, 13, 14, 22, 23, 27, 33, 39 (39 is input only) */

#endif /* HALTZ_IO_BOARD_PINS_H */
