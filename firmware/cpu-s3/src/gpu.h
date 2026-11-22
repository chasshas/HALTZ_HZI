/*
 * HALTZ CPU: driver for the GPU link to Pico #1 (docs/gpu-protocol-v1.md, S3 side: section 7).
 *
 * Use:
 *     gpu_start();                       once, at boot: pins, SPI, shadow, driver task
 *     gpu_text_print(0, 0, HALTZ_COLOR_WHITE, 0, "hello");
 *     gpu_present();                     the panel changes only here
 *     gpu_vsync(100);                    optional: wait until it is on the panel
 *
 * - The gpu_*() drawing calls may be used from any task. They check the values with the
 *   shared pack rules (HALTZ_ERR_BAD_VALUE: nothing is sent), update the shadow copy of the
 *   GPU picture in PSRAM, and append the frame to a 4 KB DMA buffer. A buffer goes out when it
 *   is full, at gpu_present(), or 2 ms after its first frame. They block only while both
 *   buffers are busy or the GPU raises BUSY (backpressure), never for a reply.
 * - The driver task (core 0) does the boot handshake, sends GPU_STATUS_REQ every 250 ms,
 *   collects the GPU's replies from MISO, and resets the GPU when it stops answering. After any
 *   GPU reset or reboot it sends GPU_INIT and replays the shadow, so the picture comes back
 *   by itself. While the link is down the drawing calls still update the shadow; nothing is
 *   lost and nobody has to wait.
 * - Pacing (protocol section 3, Frames): one PRESENT in flight at a time. gpu_present() waits
 *   until the previous PRESENT is on the panel (FRAME pulse) before it sends the next one.
 */
#ifndef HALTZ_CPU_GPU_H
#define HALTZ_CPU_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "haltz/gpu.h"
#include "haltz/gpu_msgs.h"
#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI clock at boot. The stress test passed at 10 MHz on the boards (2026-11-22). If the link
 * ever misbehaves (new wiring), go back to HALTZ_GPU_SPI_HZ_START (4 MHz). */
#define GPU_SPI_HZ_BOOT HALTZ_GPU_SPI_HZ_MAX

typedef enum {
    GPU_LINK_RESET,     /* holding / releasing the Pico's RUN line */
    GPU_LINK_HANDSHAKE, /* HELLO every 50 ms, waiting for the GPU's HELLO */
    GPU_LINK_UP,        /* picture on the GPU, drawing calls go out */
    GPU_LINK_MISSING,   /* 3 resets without an answer; tried again every 10 s */
} gpu_link_state_t;

typedef struct {
    gpu_link_state_t state;
    uint32_t spi_hz;
    /* The GPU's last HELLO (proto 0 = none seen yet). A version other than
     * HALTZ_PROTO_VERSION is shown as a warning but the link is used anyway. */
    uint16_t peer_proto;
    char peer_git[9];
    uint32_t resets;      /* hard resets (RUN pulled low) since boot */
    uint32_t link_ups;    /* times the picture was sent after a handshake */
    const char *last_reset_reason; /* why the GPU was last reset / resynced, NULL if never */
    /* Since the link last came up: */
    uint32_t presents;    /* PRESENTs sent (including the one after the replay) */
    uint32_t frames;      /* FRAME pulses seen */
    /* Since boot: */
    uint32_t naks;        /* NAKs from the GPU (a S3 bug or line corruption) */
    uint32_t miso_crc_errors; /* bad frames on MISO */
    uint32_t busy_waits;  /* transactions that had to wait for BUSY to drop */
    uint32_t transactions;
    uint64_t tx_bytes;
    /* The GPU's last GPU_STATUS (valid if have_status) and its age. */
    bool have_status;
    haltz_gpu_status_t status;
    uint32_t status_age_ms;
} gpu_stats_t;

/* ---- driver ------------------------------------------------------------------------------ */

/* Pins, SPI host A, the shadow (PSRAM) and the driver task. Returns at once; the handshake
 * runs in the task. Call once. */
esp_err_t gpu_start(void);

bool gpu_is_up(void);
/* Wait until the link is up (true) or timeout_ms passed (false). */
bool gpu_wait_up(uint32_t timeout_ms);
void gpu_get_stats(gpu_stats_t *out);
const char *gpu_link_state_name(gpu_link_state_t s);

/* Change the SPI clock (1 MHz .. HALTZ_GPU_SPI_HZ_MAX). Waits for the transaction in flight. */
esp_err_t gpu_set_spi_hz(uint32_t hz);
/* Hard-reset the GPU now; the driver redoes the handshake and replays the picture. */
void gpu_force_reset(void);

/* ---- frames ------------------------------------------------------------------------------ */

/* GPU_PRESENT: show everything drawn so far. Waits while the previous PRESENT is not on the
 * panel yet (at most about the frame timeout; the driver resets the GPU after that). Returns
 * the PRESENT id. While the link is down: returns at once. */
uint16_t gpu_present(void);
/* Wait until every PRESENT sent so far is on the panel. False on timeout or while the link is
 * down (then it sleeps one 60 Hz frame first, so a program's frame loop keeps its pace). */
bool gpu_vsync(uint32_t timeout_ms);

/* ---- drawing (protocol section 4). All return HALTZ_OK or the pack error. ---------------- */

/* Any S3 -> GPU command except GPU_PRESENT and GPU_STATUS_REQ (HALTZ_ERR_ARG). */
haltz_err_t gpu_cmd(const haltz_gpu_cmd_t *cmd);

haltz_err_t gpu_init_picture(void); /* GPU_INIT: all layers back to the defaults */
haltz_err_t gpu_backlight(uint8_t level);
haltz_err_t gpu_layers(uint8_t mask);
/* n colours from first; split into as many GPU_PALETTE messages as needed. */
haltz_err_t gpu_palette(uint8_t first, const uint16_t *colors, size_t n);

/* n characters from (col, row), wrapping to the next rows, stopping after row 29. */
haltz_err_t gpu_text_put(uint8_t col, uint8_t row, uint8_t fg, uint8_t bg, const uint8_t *chars,
                         size_t n);
haltz_err_t gpu_text_print(uint8_t col, uint8_t row, uint8_t fg, uint8_t bg, const char *s);
haltz_err_t gpu_text_fill(uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t ch, uint8_t fg,
                          uint8_t bg);
haltz_err_t gpu_text_scroll(uint8_t top, uint8_t height, int8_t rows, uint8_t ch, uint8_t fg,
                            uint8_t bg);
haltz_err_t gpu_text_cursor(uint8_t col, uint8_t row, uint8_t style);
haltz_err_t gpu_font_load(uint8_t first, const uint8_t (*glyphs)[HALTZ_GPU_GLYPH_BYTES],
                          size_t n);

haltz_err_t gpu_pattern_load(uint8_t index, const uint8_t data[HALTZ_GPU_PATTERN_BYTES]);
/* n map entries from (x, y), wrapping to the next map rows, stopping at the end of the map. */
haltz_err_t gpu_tile_put(uint8_t x, uint8_t y, const uint16_t *entries, size_t n);
haltz_err_t gpu_tile_fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t entry);
haltz_err_t gpu_tile_scroll(uint16_t sx, uint16_t sy);
haltz_err_t gpu_sprite_set(uint8_t id, int16_t x, int16_t y, uint8_t pattern, uint8_t attr);

/* n pixels from (x, y) to the right, one row, clipped at the right edge. */
haltz_err_t gpu_bmp_pixels(uint16_t x, uint16_t y, const uint8_t *pixels, size_t n);
haltz_err_t gpu_bmp_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);
haltz_err_t gpu_bmp_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
haltz_err_t gpu_bmp_copy(uint16_t sx, uint16_t sy, uint16_t w, uint16_t h, int16_t dx,
                         int16_t dy);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_GPU_H */
