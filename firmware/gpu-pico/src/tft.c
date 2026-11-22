#include "tft.h"

#include <stdbool.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "board_pins.h"
#include "haltz/gpu.h"

#define BL_WRAP 999 /* backlight PWM: 150 MHz / 150 / 1000 = 1 kHz */

static unsigned s_baud;
static int s_dma = -1;
static bool s_16bit;

static void set_bits(unsigned bits) {
    /* spi_set_format disables the SPI while it changes, so the bus must be idle. */
    while (spi_is_busy(spi1)) {
        tight_loop_contents();
    }
    spi_set_format(spi1, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    s_16bit = (bits == 16);
}

static void write_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    if (s_16bit) {
        set_bits(8);
    }
    gpio_put(PIN_TFT_CS, 0);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(spi1, &cmd, 1);
    if (len > 0) {
        gpio_put(PIN_TFT_DC, 1);
        spi_write_blocking(spi1, data, len);
    }
    gpio_put(PIN_TFT_CS, 1);
}

static void cmd0(uint8_t cmd) {
    write_cmd(cmd, NULL, 0);
}

static void cmd1(uint8_t cmd, uint8_t v) {
    write_cmd(cmd, &v, 1);
}

void tft_backlight(uint8_t level) {
    pwm_set_gpio_level(PIN_TFT_BL, (uint16_t)((level * (BL_WRAP + 1u)) / 255u));
}

unsigned tft_spi_hz(void) {
    return s_baud;
}

void tft_init(void) {
    s_baud = spi_init(spi1, TFT_SPI_HZ);
    set_bits(8);
    gpio_set_function(PIN_TFT_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_TFT_MOSI, GPIO_FUNC_SPI);

    /* CS and DC by hand: DC changes in the middle of a command. */
    const uint pins[] = {PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 1);
        gpio_set_dir(pins[i], GPIO_OUT);
    }

    /* Backlight off until the panel shows black (no flash of old panel memory). */
    gpio_set_function(PIN_TFT_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_TFT_BL);
    pwm_set_clkdiv(slice, 150.0f);
    pwm_set_wrap(slice, BL_WRAP);
    tft_backlight(0);
    pwm_set_enabled(slice, true);

    gpio_put(PIN_TFT_RST, 0);
    sleep_ms(20);
    gpio_put(PIN_TFT_RST, 1);
    sleep_ms(150);

    cmd0(0x01); /* SWRESET */
    sleep_ms(150);
    cmd0(0x11); /* SLPOUT */
    sleep_ms(120);
    cmd1(0x3A, 0x55); /* COLMOD: 16 bit/pixel */
    cmd1(0x36, (uint8_t)(TFT_MADCTL | (TFT_BGR ? 0x08 : 0x00)));
    cmd0(TFT_INVERT ? 0x21 : 0x20); /* INVON / INVOFF */
    cmd0(0x13);                     /* NORON */

    /* DMA channel for pixels: 16-bit transfers into the SPI data register. */
    s_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(spi1, true));
    dma_channel_configure((uint)s_dma, &c, &spi_get_hw(spi1)->dr, NULL, 0, false);

    /* Clear to black, then light up. */
    static uint16_t black[HALTZ_GPU_SCREEN_W];
    tft_begin_rect(0, 0, HALTZ_GPU_SCREEN_W, HALTZ_GPU_SCREEN_H);
    for (int y = 0; y < HALTZ_GPU_SCREEN_H; y++) {
        tft_send(black, HALTZ_GPU_SCREEN_W);
    }
    tft_end_rect();
    cmd0(0x29); /* DISPON */
    sleep_ms(20);
    tft_backlight(255);
}

static void wait_dma(void) {
    dma_channel_wait_for_finish_blocking((uint)s_dma);
}

void tft_begin_rect(int x, int y, int w, int h) {
    uint16_t x1 = (uint16_t)(x + w - 1);
    uint16_t y1 = (uint16_t)(y + h - 1);
    uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ra[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
    write_cmd(0x2A, ca, 4); /* CASET */
    write_cmd(0x2B, ra, 4); /* RASET */

    uint8_t ramwr = 0x2C;
    gpio_put(PIN_TFT_CS, 0);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(spi1, &ramwr, 1);
    gpio_put(PIN_TFT_DC, 1);
    set_bits(16); /* waits until RAMWR is out; CS stays low */
}

void tft_send(const uint16_t *pixels, size_t count) {
    wait_dma(); /* the previous buffer is done */
    dma_channel_transfer_from_buffer_now((uint)s_dma, pixels, (uint32_t)count);
}

void tft_end_rect(void) {
    wait_dma();
    while (spi_is_busy(spi1)) {
        tight_loop_contents();
    }
    /* The SPI also "receives" while we only transmit (MISO isn't wired): drop those words
     * and clear the overrun flag, as spi_write_blocking does. */
    while (spi_is_readable(spi1)) {
        (void)spi_get_hw(spi1)->dr;
    }
    spi_get_hw(spi1)->icr = SPI_SSPICR_RORIC_BITS;
    gpio_put(PIN_TFT_CS, 1);
}
