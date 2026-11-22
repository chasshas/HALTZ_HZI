/*
 * GPU end of the S3 link (docs/gpu-protocol-v1.md section 1). Core 0 only.
 *
 * SPI0 slave, mode 3. Both directions are byte streams:
 *   RX: DMA writes every received byte into a 32 KB ring, endlessly. A byte counter (DMA
 *       transfer count plus re-arms) tells exactly how much arrived, so overflows are seen.
 *   TX: queued frames are sent by one DMA channel, which chains to a second channel that
 *       sends 0x00 filler forever. Queuing a frame stops the filler and starts the frame.
 *   BUSY: high while not ready, or while the ring has less than HALTZ_GPU_BUSY_FREE free.
 *   FRAME: one pulse per PRESENT (link_frame_pulse, safe from either core).
 */
#ifndef HALTZ_GPU_LINK_H
#define HALTZ_GPU_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pins, SPI0 slave and DMA. BUSY stays high until link_set_ready(true). */
void link_init(void);
void link_set_ready(bool ready);

/*
 * Received bytes. link_rx_peek gives the longest contiguous run of unread bytes (may be
 * less than all of them at the ring's wrap point), link_rx_consume releases n of them.
 * *overflow is set (and the unread bytes are dropped) if the S3 overran the ring.
 */
size_t link_rx_peek(const uint8_t **data, bool *overflow);
void link_rx_consume(size_t n);

/* True between transactions with every received byte consumed: the decoder may flush. */
bool link_rx_idle_between_transactions(void);

/* Queue one frame for the S3. False if the send queue is full (frame dropped). */
bool link_send(uint8_t type, const uint8_t *payload, size_t len);

/* Call often: starts queued frames, updates BUSY. */
void link_poll(void);

void link_frame_pulse(void);

/* Status values for GPU_STATUS. */
bool link_busy(void);
uint32_t link_rx_free(void);
uint32_t link_rx_overruns(void);

#endif /* HALTZ_GPU_LINK_H */
