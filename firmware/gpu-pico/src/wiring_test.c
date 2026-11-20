/*
 * HALTZ GPU (Pico 2 W #1) - wiring test firmware (target haltz_gpu_wiring).
 * Not the real GPU firmware: flash haltz_gpu_wiring.uf2 only to check the wires.
 *
 * Stage 2, TFT (core 1): loops a test pattern forever. Full red, green, blue, white, black,
 *   then colour bars with corner markers, then a backlight fade. The console says which
 *   colour should be on screen, so a wrong colour or a dark screen points at a wire.
 *
 * Stage 3, link to the S3 (core 0): the S3's wiring test drives this side.
 *   - FRAME (GP21) always copies the level on CS (GP17), BUSY (GP20) copies SCK (GP18).
 *     The S3 drives CS and SCK as plain pins and reads them back over FRAME and BUSY.
 *   - SPI0 runs as a slave, mode 3 (CPOL=1, CPHA=1), 8 bits. Every block of 8 bytes that
 *     arrives on RX (GP16) is sent back inverted on TX (GP19) during the next block.
 *   - Once a second the console shows what arrived: edges seen on each line (sampled by a
 *     loop, so fast SPI clocks are undercounted) and the last block received.
 *
 * Stage 6: this console is UART0 (GP0/GP1) to the debug probe. If you can read it, the
 *   probe UART wire Pico #1 GP0 -> probe GP5 works.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "board_pins.h"
#include "haltz/proto.h"

#if defined(__has_include)
#if __has_include("haltz_git_hash.h")
#include "haltz_git_hash.h"
#endif
#endif
#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

/* ---- TFT settings (change here if the picture looks wrong) ------------------------------ */
#define TFT_SPI_HZ  16000000 /* wiring doc: start at 10-20 MHz */
#define TFT_W       320      /* landscape */
#define TFT_H       240
#define TFT_MADCTL  0x60 /* MX | MV: landscape. Try 0xA0 if the picture is upside down */
#define TFT_INVERT  1    /* most ST7789V IPS panels need INVON. 0 if colours are inverted */
#define TFT_BGR     0    /* 1 if red and blue are swapped (adds MADCTL BGR bit) */
#define TFT_HOLD_MS 1200 /* time per colour */

/* RGB565 */
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F

static void tft_write_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    gpio_put(PIN_TFT_CS, 0);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(spi1, &cmd, 1);
    if (len > 0) {
        gpio_put(PIN_TFT_DC, 1);
        spi_write_blocking(spi1, data, len);
    }
    gpio_put(PIN_TFT_CS, 1);
}

static void tft_cmd0(uint8_t cmd) {
    tft_write_cmd(cmd, NULL, 0);
}

static void tft_cmd1(uint8_t cmd, uint8_t v) {
    tft_write_cmd(cmd, &v, 1);
}

static void tft_init(void) {
    uint baud = spi_init(spi1, TFT_SPI_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_TFT_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_TFT_MOSI, GPIO_FUNC_SPI);

    /* CS and DC by hand: DC has to change in the middle of a command. */
    const uint pins[] = {PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 1);
        gpio_set_dir(pins[i], GPIO_OUT);
    }

    /* Backlight: PWM, ~1 kHz, start fully on. */
    gpio_set_function(PIN_TFT_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_TFT_BL);
    pwm_set_clkdiv(slice, 150.0f);
    pwm_set_wrap(slice, 999);
    pwm_set_gpio_level(PIN_TFT_BL, 1000);
    pwm_set_enabled(slice, true);

    /* Hardware reset: tests the RES wire. Without it, the SWRESET below still works. */
    gpio_put(PIN_TFT_RST, 0);
    sleep_ms(20);
    gpio_put(PIN_TFT_RST, 1);
    sleep_ms(150);

    tft_cmd0(0x01); /* SWRESET */
    sleep_ms(150);
    tft_cmd0(0x11); /* SLPOUT */
    sleep_ms(120);
    tft_cmd1(0x3A, 0x55); /* COLMOD: 16 bit/pixel */
    tft_cmd1(0x36, (uint8_t)(TFT_MADCTL | (TFT_BGR ? 0x08 : 0x00)));
    tft_cmd0(TFT_INVERT ? 0x21 : 0x20); /* INVON / INVOFF */
    tft_cmd0(0x13);                     /* NORON */
    tft_cmd0(0x29);                     /* DISPON */
    sleep_ms(20);

    printf("tft: SPI1 at %u Hz, %dx%d, MADCTL 0x%02X, inversion %s\n", baud, TFT_W, TFT_H,
           TFT_MADCTL, TFT_INVERT ? "on" : "off");
}

static void tft_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    tft_write_cmd(0x2A, ca, 4); /* CASET */
    tft_write_cmd(0x2B, ra, 4); /* RASET */
}

static void tft_fill_rect(int x, int y, int w, int h, uint16_t color) {
    static uint8_t line[TFT_W * 2];
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int i = 0; i < w; i++) {
        line[2 * i] = (uint8_t)(color >> 8);
        line[2 * i + 1] = (uint8_t)color;
    }
    tft_window((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    uint8_t ramwr = 0x2C;
    gpio_put(PIN_TFT_CS, 0);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(spi1, &ramwr, 1);
    gpio_put(PIN_TFT_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_write_blocking(spi1, line, (size_t)w * 2);
    }
    gpio_put(PIN_TFT_CS, 1);
}

static void tft_bars(void) {
    static const uint16_t bars[] = {C_WHITE, C_YELLOW, C_CYAN,    C_GREEN,
                                    C_MAGENTA, C_RED,  C_BLUE, C_BLACK};
    const int n = (int)(sizeof(bars) / sizeof(bars[0]));
    for (int i = 0; i < n; i++) {
        int x0 = i * TFT_W / n;
        int x1 = (i + 1) * TFT_W / n;
        tft_fill_rect(x0, 0, x1 - x0, TFT_H, bars[i]);
    }
    /* 1-pixel white frame: all four edges visible = no offset problem. */
    tft_fill_rect(0, 0, TFT_W, 1, C_WHITE);
    tft_fill_rect(0, TFT_H - 1, TFT_W, 1, C_WHITE);
    tft_fill_rect(0, 0, 1, TFT_H, C_WHITE);
    tft_fill_rect(TFT_W - 1, 0, 1, TFT_H, C_WHITE);
    /* Corner markers: red = top-left, green = top-right, blue = bottom-left. */
    tft_fill_rect(4, 4, 24, 24, C_RED);
    tft_fill_rect(TFT_W - 28, 4, 24, 24, C_GREEN);
    tft_fill_rect(4, TFT_H - 28, 24, 24, C_BLUE);
}

static void core1_tft_loop(void) {
    static const struct {
        uint16_t color;
        const char *name;
    } fills[] = {{C_RED, "red"},     {C_GREEN, "green"}, {C_BLUE, "blue"},
                 {C_WHITE, "white"}, {C_BLACK, "black"}};

    tft_init();
    uint32_t round = 0;
    while (true) {
        round++;
        for (size_t i = 0; i < sizeof(fills) / sizeof(fills[0]); i++) {
            if (round == 1) {
                printf("tft: screen should be %s\n", fills[i].name);
            }
            tft_fill_rect(0, 0, TFT_W, TFT_H, fills[i].color);
            sleep_ms(TFT_HOLD_MS);
        }
        if (round == 1) {
            printf("tft: colour bars white|yellow|cyan|green|magenta|red|blue|black, white "
                   "frame, corner squares red=top-left green=top-right blue=bottom-left\n");
        }
        tft_bars();
        sleep_ms(3 * TFT_HOLD_MS);
        if (round == 1) {
            printf("tft: backlight fading down and up (BL wire)\n");
        }
        for (int l = 1000; l >= 0; l -= 20) {
            pwm_set_gpio_level(PIN_TFT_BL, (uint16_t)l);
            sleep_ms(15);
        }
        for (int l = 0; l <= 1000; l += 20) {
            pwm_set_gpio_level(PIN_TFT_BL, (uint16_t)l);
            sleep_ms(15);
        }
        if (round == 1) {
            printf("tft: pattern repeats (quiet from now on)\n");
        }
    }
}

/* ---- Link to the S3 ---------------------------------------------------------------------- */

#define LINK_BLOCK 8 /* = SPI FIFO depth */

static void link_init(void) {
    spi_init(spi0, 1000000); /* rate is set by the S3; slave only needs clk_peri >= 12x SCK */
    spi_set_slave(spi0, true);
    spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_LINK_RX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LINK_CSN, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LINK_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LINK_TX, GPIO_FUNC_SPI);

    gpio_init(PIN_LINK_BUSY);
    gpio_set_dir(PIN_LINK_BUSY, GPIO_OUT);
    gpio_init(PIN_LINK_FRAME);
    gpio_set_dir(PIN_LINK_FRAME, GPIO_OUT);
}

int main(void) {
    stdio_init_all(); /* UART0 GP0/GP1, 115200 8N1 -> debug probe */

    /* Mirror right away so the S3 sees us as soon as possible after a reset. */
    link_init();
    uint32_t in = gpio_get_all();
    gpio_put(PIN_LINK_FRAME, (in >> PIN_LINK_CSN) & 1u);
    gpio_put(PIN_LINK_BUSY, (in >> PIN_LINK_SCK) & 1u);

    printf("\n==========================================\n");
    printf(" HALTZ GPU - WIRING TEST\n");
    printf(" protocol v%u, git %s\n", (unsigned)HALTZ_PROTO_VERSION, HALTZ_GIT_HASH);
    printf("==========================================\n");
    printf("stage 6: you are reading this over the probe UART, so GP0 -> probe works\n");
    printf("link: FRAME(GP21) copies CS(GP17), BUSY(GP20) copies SCK(GP18), "
           "SPI0 slave mode 3 echoes 8-byte blocks inverted\n");

    multicore_launch_core1(core1_tft_loop);

    uint8_t blk[LINK_BLOCK];
    uint8_t last_blk[LINK_BLOCK] = {0};
    int n = 0;
    uint32_t blocks = 0, partial = 0, tx_full = 0;
    uint32_t edges_cs = 0, edges_sck = 0, edges_rx = 0;
    uint32_t prev = in;
    uint32_t rep_blocks = 0, rep_cs = 0, rep_sck = 0, rep_rx = 0, rep_partial = 0;
    absolute_time_t next_report = make_timeout_time_ms(1000);

    while (true) {
        in = gpio_get_all();
        gpio_put(PIN_LINK_FRAME, (in >> PIN_LINK_CSN) & 1u);
        gpio_put(PIN_LINK_BUSY, (in >> PIN_LINK_SCK) & 1u);
        uint32_t ch = in ^ prev;
        edges_cs += (ch >> PIN_LINK_CSN) & 1u;
        edges_sck += (ch >> PIN_LINK_SCK) & 1u;
        edges_rx += (ch >> PIN_LINK_RX) & 1u;
        prev = in;

        while (spi_is_readable(spi0)) {
            blk[n++] = (uint8_t)spi_get_hw(spi0)->dr;
            if (n == LINK_BLOCK) {
                for (int i = 0; i < LINK_BLOCK; i++) {
                    if (spi_is_writable(spi0)) {
                        spi_get_hw(spi0)->dr = (uint8_t)~blk[i];
                    } else {
                        tx_full++;
                    }
                    last_blk[i] = blk[i];
                }
                blocks++;
                n = 0;
            }
        }
        /* CS high in the middle of a block: the S3 sent a short transfer. Start over. */
        if (n != 0 && ((in >> PIN_LINK_CSN) & 1u)) {
            busy_wait_us(5); /* let the last byte reach the RX FIFO */
            if (!spi_is_readable(spi0)) {
                partial++;
                n = 0;
            }
        }

        if (time_reached(next_report)) {
            next_report = make_timeout_time_ms(1000);
            if (edges_cs != rep_cs || edges_sck != rep_sck || edges_rx != rep_rx ||
                blocks != rep_blocks || partial != rep_partial) {
                printf("link: last 1 s: CS %lu edges, SCK %lu, MOSI(RX) %lu | blocks %lu "
                       "total, last:",
                       (unsigned long)(edges_cs - rep_cs), (unsigned long)(edges_sck - rep_sck),
                       (unsigned long)(edges_rx - rep_rx), (unsigned long)blocks);
                for (int i = 0; i < LINK_BLOCK; i++) {
                    printf(" %02X", last_blk[i]);
                }
                printf(" | short transfers %lu, tx fifo full %lu\n", (unsigned long)partial,
                       (unsigned long)tx_full);
                rep_cs = edges_cs;
                rep_sck = edges_sck;
                rep_rx = edges_rx;
                rep_blocks = blocks;
                rep_partial = partial;
            }
        }
    }
}
