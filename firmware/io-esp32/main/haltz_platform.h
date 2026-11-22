/*
 * HALTZ I/O: HALTZ's Bluepad32 platform (docs/io-protocol-v1.md section 8).
 *
 * Replaces the template's Arduino platform. Bluepad32 calls it on core 0 (the BTstack run
 * loop) for every controller report; keyboard reports and device events go into a queue that
 * the io_link task (core 1) drains. Nothing here touches the UART.
 */
#ifndef HALTZ_IO_PLATFORM_H
#define HALTZ_IO_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "haltz/io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_EV_KEYS,       /* a keyboard report */
    BT_EV_CONNECT,    /* a device is ready */
    BT_EV_DISCONNECT, /* a device is gone */
    BT_EV_BATTERY,    /* battery level changed */
} bt_ev_type_t;

typedef struct {
    uint8_t type; /* bt_ev_type_t */
    uint8_t slot; /* 0..HALTZ_IO_MAX_DEVICES-1 */
    uint8_t kind; /* haltz_io_dev_kind_t */
    uint8_t battery;
    uint16_t vendor_id, product_id;
    uint8_t modifiers;                /* BT_EV_KEYS */
    uint8_t keys[HALTZ_IO_KEYS_MAX]; /* BT_EV_KEYS: 0 = empty entry */
} bt_event_t;

struct uni_platform;
/* For uni_platform_set_custom(), before uni_init(). Creates the queue. */
struct uni_platform *haltz_platform_get(void);

/* Next event, false if none (io_link task). */
bool haltz_bt_next(bt_event_t *ev);
/* Events dropped because the queue was full (the refresh repairs the key state). */
uint32_t haltz_bt_dropped(void);
/* Bluetooth scanning for new devices (from Bluepad32's OOB event). */
bool haltz_bt_scanning(void);
/* IO_BT actions; thread safe (they run on the BTstack thread). */
void haltz_bt_action(uint8_t action);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_PLATFORM_H */
