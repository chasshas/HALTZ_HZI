/*
 * HALTZ I/O: the UART link to the S3 (docs/io-protocol-v1.md, I/O side: section 8).
 *
 * io_start() brings up everything that isn't Bluetooth: buzzers and LED (io_fx.c), stick and
 * knob (io_input.c), UART2, and the io_link task on core 1, which sends HELLO at once.
 * Call it before Bluetooth starts, so the S3 hears from the board early.
 */
#ifndef HALTZ_IO_LINK_H
#define HALTZ_IO_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "haltz/io_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t io_start(void);

typedef struct {
    uint32_t uptime_ms;
    uint32_t s3_hellos;   /* HELLOs from the S3 */
    uint16_t s3_proto;    /* its version (0 = none yet) */
    char s3_git[9];
    uint32_t last_rx_age_ms; /* since the last good frame from the S3 */
    haltz_io_status_t status; /* what the next IO_STATUS will say (counters, flags) */
    uint32_t tx_frames, tx_bytes, naks_sent;
    uint32_t bt_dropped;      /* Bluetooth events lost (queue full) */
    haltz_io_keys_t keys;     /* merged keyboard state */
    uint8_t device_kind[HALTZ_IO_MAX_DEVICES];
    uint8_t device_battery[HALTZ_IO_MAX_DEVICES];
} io_link_stats_t;

void io_link_get_stats(io_link_stats_t *out);
/* Print every input message sent (not the repeats) and every command received. */
void io_link_set_watch(bool on);
bool io_link_watch(void);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_IO_LINK_H */
