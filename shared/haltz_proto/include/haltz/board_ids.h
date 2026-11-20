/* HALTZ board identifiers, sent as u8 (e.g. in HELLO). Values are part of the protocol. */
#ifndef HALTZ_BOARD_IDS_H
#define HALTZ_BOARD_IDS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HALTZ_BOARD_CPU = 1, /* ESP32-S3: kernel + HALTZ VM */
    HALTZ_BOARD_IO = 2,  /* ESP32: Bluetooth keyboard, thumbstick, knob, buzzers, RGB LED */
    HALTZ_BOARD_GPU = 3, /* Pico 2 W: ST7789V TFT */
} haltz_board_id_t;

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_BOARD_IDS_H */
