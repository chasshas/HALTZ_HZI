#include "link.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "board_pins.h"
#include "haltz/gpu.h"
#include "haltz/packet.h"

/* ---- receive ring ------------------------------------------------------------------------ */

#define RX_RING_BITS 15
#define RX_RING      (1u << RX_RING_BITS) /* = HALTZ_GPU_RX_RING */
#define RX_ARM_COUNT 0x0FFFFFFFu          /* transfers per DMA arm (~214 s at 10 MHz) */
#define RX_SAFETY    64                   /* treat a ring this close to full as overrun */

_Static_assert(RX_RING == HALTZ_GPU_RX_RING, "ring size");

/* The DMA ring wrap needs the buffer aligned to its size. */
static uint8_t s_rx_ring[RX_RING] __attribute__((aligned(RX_RING)));
static volatile uint32_t s_rx_base; /* bytes received before the current DMA arm */
static uint32_t s_rd;               /* bytes consumed (wraps with the counter) */
static uint32_t s_overruns;
static uint s_rx_ch;

/* ---- send queue -------------------------------------------------------------------------- */

#define TX_Q 1024u /* power of two */

static uint8_t s_txq[TX_Q];
static uint32_t s_tx_head;     /* bytes queued (frames end here) */
static uint32_t s_tx_tail;     /* bytes sent */
static uint32_t s_tx_inflight; /* bytes the data channel is sending now */
static uint8_t s_filler = HALTZ_GPU_FILLER;
static uint s_tx_ch, s_fill_ch;

static bool s_ready;
static bool s_busy = true;

/* ---- RX ---------------------------------------------------------------------------------- */

static void __isr rx_dma_irq(void) {
    if (dma_channel_get_irq0_status(s_rx_ch)) {
        dma_channel_acknowledge_irq0(s_rx_ch);
        s_rx_base += RX_ARM_COUNT;
        dma_channel_set_trans_count(s_rx_ch, RX_ARM_COUNT, true); /* continue where it was */
    }
}

/* Total bytes received so far (wraps at 2^32, like s_rd). */
static uint32_t rx_total(void) {
    uint32_t save = save_and_disable_interrupts();
    uint32_t rem = dma_hw->ch[s_rx_ch].transfer_count & 0x0FFFFFFFu;
    uint32_t total = s_rx_base + (RX_ARM_COUNT - rem);
    restore_interrupts(save);
    /* The count drops when a transfer is issued; give its write to RAM time to land. */
    busy_wait_at_least_cycles(16);
    return total;
}

size_t link_rx_peek(const uint8_t **data, bool *overflow) {
    uint32_t total = rx_total();
    uint32_t avail = total - s_rd;
    *overflow = false;
    if (avail > RX_RING - RX_SAFETY) {
        /* The S3 sent more than BUSY allowed: the oldest bytes are already overwritten. */
        s_overruns++;
        s_rd = total;
        *overflow = true;
        return 0;
    }
    uint32_t off = s_rd & (RX_RING - 1);
    uint32_t n = RX_RING - off;
    *data = &s_rx_ring[off];
    return avail < n ? avail : n;
}

void link_rx_consume(size_t n) {
    s_rd += (uint32_t)n;
}

bool link_rx_idle_between_transactions(void) {
    /* Order matters: CS high first, then nothing left in the FIFO, then all consumed. */
    return gpio_get(PIN_LINK_CSN) && !spi_is_readable(spi0) && rx_total() == s_rd;
}

uint32_t link_rx_free(void) {
    uint32_t used = rx_total() - s_rd;
    return used >= RX_RING ? 0 : RX_RING - used;
}

uint32_t link_rx_overruns(void) {
    return s_overruns;
}

/* ---- TX ---------------------------------------------------------------------------------- */

/* Start the next queued bytes if the data channel is free. Queued bytes always end on a frame
 * boundary, and a frame never wraps (see link_send), so the filler that runs between two
 * chunks can only land between frames. */
static void tx_pump(void) {
    if (dma_channel_is_busy(s_tx_ch)) {
        return;
    }
    s_tx_tail += s_tx_inflight;
    s_tx_inflight = 0;
    uint32_t pending = s_tx_head - s_tx_tail;
    if (pending == 0) {
        return;
    }
    uint32_t off = s_tx_tail & (TX_Q - 1);
    uint32_t n = pending < TX_Q - off ? pending : TX_Q - off;

    /* Stop the filler. RP2350-E5: clear EN before the abort so nothing re-triggers it, then
     * enable it again so the data channel can chain to it. */
    hw_clear_bits(&dma_hw->ch[s_fill_ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(s_fill_ch);
    hw_set_bits(&dma_hw->ch[s_fill_ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_set_trans_count(s_fill_ch, dma_encode_endless_transfer_count(), false);

    dma_channel_set_read_addr(s_tx_ch, &s_txq[off], false);
    dma_channel_set_trans_count(s_tx_ch, n, true);
    s_tx_inflight = n;
}

bool link_send(uint8_t type, const uint8_t *payload, size_t len) {
    uint8_t frame[HALTZ_MAX_FRAME];
    size_t n = 0;
    if (haltz_frame_encode(type, payload, len, frame, sizeof(frame), &n) != HALTZ_OK) {
        return false;
    }
    uint32_t off = s_tx_head & (TX_Q - 1);
    uint32_t pad = (off + n > TX_Q) ? TX_Q - off : 0; /* frames never wrap: skip to start */
    if (TX_Q - (s_tx_head - s_tx_tail) < pad + n) {
        return false;
    }
    for (uint32_t i = 0; i < pad; i++) {
        s_txq[off + i] = HALTZ_GPU_FILLER;
    }
    s_tx_head += pad;
    off = s_tx_head & (TX_Q - 1);
    for (size_t i = 0; i < n; i++) {
        s_txq[off + i] = frame[i];
    }
    s_tx_head += (uint32_t)n;
    tx_pump();
    return true;
}

/* ---- control lines ----------------------------------------------------------------------- */

void link_set_ready(bool ready) {
    s_ready = ready;
    link_poll();
}

bool link_busy(void) {
    return s_busy;
}

void link_poll(void) {
    tx_pump();
    s_busy = !s_ready || link_rx_free() < HALTZ_GPU_BUSY_FREE;
    gpio_put(PIN_LINK_BUSY, s_busy);
}

void link_frame_pulse(void) {
    gpio_put(PIN_LINK_FRAME, 1);
    busy_wait_us(HALTZ_GPU_FRAME_PULSE_US + 1);
    gpio_put(PIN_LINK_FRAME, 0);
}

/* ---- init -------------------------------------------------------------------------------- */

void link_init(void) {
    gpio_init(PIN_LINK_BUSY);
    gpio_put(PIN_LINK_BUSY, 1);
    gpio_set_dir(PIN_LINK_BUSY, GPIO_OUT);
    gpio_init(PIN_LINK_FRAME);
    gpio_put(PIN_LINK_FRAME, 0);
    gpio_set_dir(PIN_LINK_FRAME, GPIO_OUT);

    /* The rate is set by the S3; a slave only needs clk_peri >= 12 x SCK. */
    spi_init(spi0, 1000000);
    spi_set_slave(spi0, true);
    spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_LINK_RX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LINK_CSN, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LINK_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LINK_TX, GPIO_FUNC_SPI);

    volatile void *dr = &spi_get_hw(spi0)->dr;

    /* RX: SPI -> ring, endlessly (re-armed by the IRQ every RX_ARM_COUNT bytes). */
    s_rx_ch = (uint)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_rx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RX_RING_BITS);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(s_rx_ch, &c, s_rx_ring, dr, RX_ARM_COUNT, false);
    irq_set_exclusive_handler(DMA_IRQ_0, rx_dma_irq);
    dma_channel_set_irq0_enabled(s_rx_ch, true);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(s_rx_ch);

    /* TX filler: one 0x00 byte, sent forever. */
    s_fill_ch = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(s_fill_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, true));
    dma_channel_configure(s_fill_ch, &c, dr, &s_filler, dma_encode_endless_transfer_count(),
                          true);

    /* TX data: queued frames, then chain back to the filler. */
    s_tx_ch = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(s_tx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, true));
    channel_config_set_chain_to(&c, s_fill_ch);
    dma_channel_configure(s_tx_ch, &c, dr, s_txq, 0, false);
}
