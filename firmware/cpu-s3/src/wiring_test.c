/*
 * HALTZ CPU (ESP32-S3) - wiring test (PlatformIO env "cpu_wiring", -DHALTZ_WIRING_TEST).
 * Not the real CPU firmware. Checks every wire that ends at the S3, one stage at a time,
 * and says which wire to look at when something fails. A stage that isn't wired yet just
 * shows FAIL / NOT FOUND; the other stages still run.
 *
 *  Stage 1  RC522: reset, chip version over SPI, register write/read, RST line.
 *           Power button: live press/release on the console.
 *  Stage 3  GPU link (needs haltz_gpu_wiring.uf2 on Pico #1): RESET, CS+FRAME and SCK+BUSY
 *           as plain pins (the Pico copies CS to FRAME and SCK to BUSY), then SPI mode 3
 *           (the Pico echoes each 8-byte block inverted), which checks MOSI and MISO.
 *  Stage 4  I/O link (needs the io-esp32 "esp32dev_wiring" build on the ESP32): EN reset,
 *           boot banner ESP32 -> S3, inverted echo S3 -> ESP32 -> S3.
 *
 * After the tests: live power button and RC522 card reads. Press BOOT to run them again.
 */
#ifdef HALTZ_WIRING_TEST

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"

/* Same text as firmware/io-esp32/main/haltz_wiring_test.c sends at boot. */
#define IO_BANNER "HALTZ-IO-WIRING"

#define RC522_SPI_HZ  1000000 /* slow on purpose; the RC522 can do 10 MHz */
#define GPU_SPI_HZ    1000000 /* wiring doc: start the GPU link at 1-4 MHz */
#define IO_UART_BAUD  115200
#define IO_UART_NUM   UART_NUM_1
#define PICO_READY_MS 3000 /* Pico boot + start of the mirror loop */
#define ESP_BOOT_MS   3000

typedef enum { R_PASS, R_FAIL, R_SKIP } result_t;

static const char *rname(result_t r) {
    return r == R_PASS ? "PASS" : (r == R_FAIL ? "FAIL" : "SKIP");
}

static void check(result_t *acc, bool ok, const char *what, const char *hint) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        if (hint != NULL) {
            printf("         -> %s\n", hint);
        }
        *acc = R_FAIL;
    }
}

static bool contains(const uint8_t *buf, size_t len, const char *text) {
    size_t tl = strlen(text);
    for (size_t i = 0; i + tl <= len; i++) {
        if (memcmp(buf + i, text, tl) == 0) {
            return true;
        }
    }
    return false;
}

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
}

static void out_pin(gpio_num_t pin, int level, bool open_drain) {
    gpio_reset_pin(pin);
    gpio_set_level(pin, level);
    gpio_set_direction(pin, open_drain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT);
}

static void in_pin(gpio_num_t pin, gpio_pull_mode_t pull) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, pull);
}

/* ======================================================================================== */
/* Stage 1: RC522                                                                           */
/* ======================================================================================== */

#define RC_COMMAND     0x01
#define RC_COMIRQ      0x04
#define RC_ERROR       0x06
#define RC_FIFODATA    0x09
#define RC_FIFOLEVEL   0x0A
#define RC_CONTROL     0x0C
#define RC_BITFRAMING  0x0D
#define RC_COLL        0x0E
#define RC_MODE        0x11
#define RC_TXCONTROL   0x14
#define RC_TXASK       0x15
#define RC_MODWIDTH    0x24 /* reset value 0x26, safe to scribble on for the test */
#define RC_TMODE       0x2A
#define RC_TPRESCALER  0x2B
#define RC_TRELOADH    0x2C
#define RC_TRELOADL    0x2D
#define RC_VERSION     0x37
#define RC_CMD_IDLE    0x00
#define RC_CMD_TRANSCV 0x0C
#define RC_CMD_RESET   0x0F

static spi_device_handle_t s_rc;
static bool s_rc_bus;
static bool s_rc_ok; /* chip found: poll for cards in the live loop */

static uint8_t rc_read(uint8_t reg) {
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA, .length = 16};
    t.tx_data[0] = (uint8_t)(0x80 | (reg << 1));
    t.tx_data[1] = 0;
    spi_device_polling_transmit(s_rc, &t);
    return t.rx_data[1];
}

static void rc_write(uint8_t reg, uint8_t val) {
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA, .length = 16};
    t.tx_data[0] = (uint8_t)((reg << 1) & 0x7E);
    t.tx_data[1] = val;
    spi_device_polling_transmit(s_rc, &t);
}

static void rc_hard_reset(void) {
    gpio_set_level(PIN_RC522_RST, 0);
    delay_ms(10);
    gpio_set_level(PIN_RC522_RST, 1);
    delay_ms(50); /* oscillator start-up */
}

static const char *rc_version_name(uint8_t v) {
    switch (v) {
    case 0x91: return "MFRC522 v1.0";
    case 0x92: return "MFRC522 v2.0";
    case 0x88: return "FM17522 (clone)";
    case 0x89: return "FM17522E (clone)";
    case 0xB2: return "FM17522 (clone)";
    case 0x12: return "counterfeit MFRC522 (works for most uses)";
    case 0x18: return "MFRC522 clone";
    default: return NULL;
    }
}

static result_t stage1_rc522(void) {
    result_t r = R_PASS;
    printf("\n--- Stage 1: RC522 card reader (SPI host B) ---\n");

    out_pin(PIN_RC522_RST, 1, false);
    if (!s_rc_bus) {
        spi_bus_config_t bus = {.mosi_io_num = PIN_RC522_MOSI,
                                .miso_io_num = PIN_RC522_MISO,
                                .sclk_io_num = PIN_RC522_SCK,
                                .quadwp_io_num = -1,
                                .quadhd_io_num = -1};
        spi_device_interface_config_t dev = {.clock_speed_hz = RC522_SPI_HZ,
                                             .mode = 0,
                                             .spics_io_num = PIN_RC522_CS,
                                             .queue_size = 1};
        if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK ||
            spi_bus_add_device(SPI3_HOST, &dev, &s_rc) != ESP_OK) {
            printf("  cannot start SPI host B\n");
            return R_FAIL;
        }
        /* Pull-up on MISO: an unconnected MISO then reads 0xFF instead of noise. */
        gpio_pullup_en(PIN_RC522_MISO);
        s_rc_bus = true;
    }

    rc_hard_reset();
    uint8_t v = rc_read(RC_VERSION);
    const char *name = rc_version_name(v);
    char what[80];
    /* An unlisted version that is not 0x00/0xFF still means something answers on the bus (many
     * clones report odd values): carry on, the register write/read below decides. */
    bool plausible = name != NULL || (v != 0x00 && v != 0xFF);
    snprintf(what, sizeof(what), "chip version 0x%02X %s%s%s", v, name ? "(" : "",
             name ? name : (plausible ? "(not in the known list, continuing)" : ""),
             name ? ")" : "");
    const char *hint =
        v == 0xFF ? "reads 0xFF: MISO (RC522 MISO -> S3 GPIO6) open, or the RC522 has no 3.3 V "
                    "/ GND, or RST is held low (RST -> GPIO15)"
        : v == 0x00 ? "reads 0x00: MISO shorted to GND, or SCK (GPIO4) / SDA=CS (GPIO7) open"
                    : "unknown value: check SCK/MOSI/MISO/SDA are not swapped, wires short";
    check(&r, plausible, what, hint);
    if (!plausible) {
        s_rc_ok = false;
        return r;
    }

    /* Write/read a harmless register: if this fails but the version read works, MOSI is bad. */
    static const uint8_t pat[] = {0x55, 0xAA, 0x00, 0xFF, 0x26};
    bool rw = true;
    for (size_t i = 0; i < sizeof(pat); i++) {
        rc_write(RC_MODWIDTH, pat[i]);
        uint8_t back = rc_read(RC_MODWIDTH);
        if (back != pat[i]) {
            printf("         wrote 0x%02X, read 0x%02X\n", pat[i], back);
            rw = false;
        }
    }
    check(&r, rw, "register write + read back (MOSI)",
          "MOSI (S3 GPIO5 -> RC522 MOSI) open or swapped with MISO");

    /* RST: scribble a register, pulse RST, it must be back at its reset value 0x26. */
    rc_write(RC_MODWIDTH, 0x55);
    rc_hard_reset();
    uint8_t after = rc_read(RC_MODWIDTH);
    check(&r, after == 0x26, "RST resets the chip",
          "register kept its value: RST (S3 GPIO15 -> RC522 RST) is open");

    /* Set up for card reads (ISO 14443A, 106 kbit/s, ~25 ms timeout), antenna on. */
    rc_write(RC_COMMAND, RC_CMD_RESET);
    delay_ms(50);
    rc_write(RC_TMODE, 0x80);
    rc_write(RC_TPRESCALER, 0xA9);
    rc_write(RC_TRELOADH, 0x03);
    rc_write(RC_TRELOADL, 0xE8);
    rc_write(RC_TXASK, 0x40);
    rc_write(RC_MODE, 0x3D);
    rc_write(RC_TXCONTROL, (uint8_t)(rc_read(RC_TXCONTROL) | 0x03));
    s_rc_ok = (r == R_PASS);
    if (s_rc_ok) {
        printf("  antenna on. Tap a card or tag after the tests: its UID is printed.\n");
    }
    return r;
}

/* Send bytes to a card, return the number of bytes received (or -1). */
static int rc_transceive(const uint8_t *tx, size_t tx_len, uint8_t tx_last_bits, uint8_t *rx,
                         size_t rx_max) {
    rc_write(RC_COMMAND, RC_CMD_IDLE);
    rc_write(RC_COMIRQ, 0x7F);
    rc_write(RC_FIFOLEVEL, 0x80);
    for (size_t i = 0; i < tx_len; i++) {
        rc_write(RC_FIFODATA, tx[i]);
    }
    rc_write(RC_BITFRAMING, tx_last_bits);
    rc_write(RC_COMMAND, RC_CMD_TRANSCV);
    rc_write(RC_BITFRAMING, (uint8_t)(0x80 | tx_last_bits)); /* StartSend */

    int64_t end = now_ms() + 40;
    uint8_t irq = 0;
    while (true) {
        irq = rc_read(RC_COMIRQ);
        if (irq & 0x30) { /* RxIRq | IdleIRq */
            break;
        }
        if ((irq & 0x01) || now_ms() > end) { /* TimerIRq: no card */
            rc_write(RC_BITFRAMING, 0);
            return -1;
        }
    }
    rc_write(RC_BITFRAMING, 0);
    if (rc_read(RC_ERROR) & 0x13) { /* BufferOvfl | ParityErr | ProtocolErr */
        return -1;
    }
    int n = rc_read(RC_FIFOLEVEL);
    if (n > (int)rx_max) {
        n = (int)rx_max;
    }
    for (int i = 0; i < n; i++) {
        rx[i] = rc_read(RC_FIFODATA);
    }
    return n;
}

/* WUPA + anticollision cascade level 1. Returns true and the 4 UID bytes if a card answers. */
static bool rc_read_uid(uint8_t uid[4], bool *cascade) {
    uint8_t buf[8];
    const uint8_t wupa = 0x52;
    rc_write(RC_COLL, (uint8_t)(rc_read(RC_COLL) & ~0x80));
    /* Twice: a card left in READY by the last poll ignores the first WUPA. */
    if (rc_transceive(&wupa, 1, 7, buf, sizeof(buf)) != 2 &&
        rc_transceive(&wupa, 1, 7, buf, sizeof(buf)) != 2) {
        return false;
    }
    const uint8_t anticoll[2] = {0x93, 0x20};
    if (rc_transceive(anticoll, 2, 0, buf, sizeof(buf)) != 5) {
        return false;
    }
    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4]) {
        return false; /* BCC mismatch */
    }
    memcpy(uid, buf, 4);
    *cascade = (buf[0] == 0x88);
    return true;
}

/* ======================================================================================== */
/* Stage 3: GPU link                                                                        */
/* ======================================================================================== */

static void gpu_pins_as_gpio(void) {
    out_pin(PIN_GPU_SCK, 1, false); /* mode 3 idles high */
    out_pin(PIN_GPU_MOSI, 0, false);
    out_pin(PIN_GPU_CS, 1, false);
    in_pin(PIN_GPU_MISO, GPIO_PULLDOWN_ONLY);
    in_pin(PIN_GPU_BUSY, GPIO_PULLDOWN_ONLY);
    in_pin(PIN_GPU_FRAME, GPIO_PULLDOWN_ONLY);
    out_pin(PIN_GPU_PICO_RUN, 1, true); /* released */
}

/* With CS=1 and SCK=1 a running Pico drives FRAME=1 and BUSY=1. */
static bool pico_mirroring(uint32_t timeout_ms) {
    gpio_set_level(PIN_GPU_CS, 1);
    gpio_set_level(PIN_GPU_SCK, 1);
    int64_t end = now_ms() + timeout_ms;
    do {
        if (gpio_get_level(PIN_GPU_FRAME) && gpio_get_level(PIN_GPU_BUSY)) {
            return true;
        }
        delay_ms(2);
    } while (now_ms() < end);
    return false;
}

/* Pull RUN low, check the Pico stops, release, return boot time in ms (or -1). */
static int pico_reset(bool *stopped) {
    gpio_set_level(PIN_GPU_PICO_RUN, 0);
    delay_ms(20);
    *stopped = !gpio_get_level(PIN_GPU_FRAME) && !gpio_get_level(PIN_GPU_BUSY);
    gpio_set_level(PIN_GPU_PICO_RUN, 1);
    int64_t t0 = now_ms();
    if (!pico_mirroring(PICO_READY_MS)) {
        return -1;
    }
    return (int)(now_ms() - t0);
}

static result_t stage3_gpu_link(void) {
    result_t r = R_PASS;
    printf("\n--- Stage 3: GPU link to Pico #1 (needs haltz_gpu_wiring.uf2 on the Pico) ---\n");
    gpu_pins_as_gpio();

    bool alive = pico_mirroring(PICO_READY_MS);
    if (!alive) {
        int frame = gpio_get_level(PIN_GPU_FRAME);
        int busy = gpio_get_level(PIN_GPU_BUSY);
        printf("  Pico not answering (FRAME=%d, BUSY=%d with CS=1, SCK=1).\n", frame, busy);
        if (frame || busy) {
            printf("  One line answers, so the Pico runs. %s\n",
                   frame ? "SCK (S3 GPIO12 -> Pico GP18) or BUSY (Pico GP20 -> S3 GPIO9) open."
                         : "CS (S3 GPIO10 -> Pico GP17) or FRAME (Pico GP21 -> S3 GPIO14) open.");
            r = R_FAIL;
        } else {
            printf("  Trying a reset in case the Pico is stuck...\n");
            bool stopped;
            if (pico_reset(&stopped) < 0) {
                printf("  -> Is Pico #1 powered and running haltz_gpu_wiring.uf2? Is GND joined?\n"
                       "     If it is: FRAME and BUSY are both open, or CS and SCK are.\n");
                return R_SKIP;
            }
            printf("  Pico answers after the reset.\n");
        }
        if (r == R_FAIL) {
            return r;
        }
    }

    /* RESET line */
    bool stopped = false;
    int boot_ms = pico_reset(&stopped);
    char what[96];
    check(&r, stopped, "RESET holds the Pico in reset (FRAME/BUSY go low)",
          "Pico kept running: RESET (S3 GPIO21 -> Pico RUN, pin 30) is open");
    snprintf(what, sizeof(what), "Pico runs again after RESET is released (%d ms)", boot_ms);
    check(&r, boot_ms >= 0, what, "RUN shorted to GND?");
    if (boot_ms < 0) {
        return r;
    }

    /* CS -> FRAME and SCK -> BUSY as plain pins, every combination. */
    static const int combos[][2] = {{1, 1}, {0, 1}, {1, 0}, {0, 0}, {1, 1}, {0, 0}, {1, 1}};
    bool cs_ok = true, sck_ok = true, swapped = true;
    for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
        int cs = combos[i][0], sck = combos[i][1];
        gpio_set_level(PIN_GPU_CS, cs);
        gpio_set_level(PIN_GPU_SCK, sck);
        /* The Pico copies within microseconds, but can pause ~15 ms while it prints. */
        int64_t end = now_ms() + 50;
        int frame, busy;
        do {
            delay_ms(1);
            frame = gpio_get_level(PIN_GPU_FRAME);
            busy = gpio_get_level(PIN_GPU_BUSY);
        } while ((frame != cs || busy != sck) && now_ms() < end);
        cs_ok &= (frame == cs);
        sck_ok &= (busy == sck);
        swapped &= (frame == sck && busy == cs);
    }
    if (swapped && !(cs_ok && sck_ok)) {
        printf("         FRAME follows SCK and BUSY follows CS: two of these wires are swapped\n");
    }
    check(&r, cs_ok, "CS (S3 GPIO10 -> Pico GP17) and FRAME (Pico GP21 -> S3 GPIO14)",
          "one of the two is open or on the wrong pin");
    check(&r, sck_ok, "SCK (S3 GPIO12 -> Pico GP18) and BUSY (Pico GP20 -> S3 GPIO9)",
          "one of the two is open or on the wrong pin");

    /* Toggle MOSI as a pin too: the Pico console counts its edges (helps if SPI fails). */
    gpio_set_level(PIN_GPU_CS, 1);
    for (int i = 0; i < 10; i++) {
        gpio_set_level(PIN_GPU_MOSI, i & 1);
        delay_ms(2);
    }
    gpio_set_level(PIN_GPU_MOSI, 0);
    if (!(cs_ok && sck_ok)) {
        printf("  SPI test skipped: CS and SCK must work first.\n");
        return r;
    }

    /* Clean SPI state on the Pico (the pin wiggling above clocked junk into it). */
    if (pico_reset(&stopped) < 0) {
        check(&r, false, "Pico back after second reset", NULL);
        return r;
    }

    /* The Pico mirrors CS/SCK right after reset but then prints its banner (tens of ms) before
     * it serves SPI. Wait that out, or the first echoes come back late. */
    delay_ms(500);

    /* SPI mode 3. The Pico sends block k-1 inverted while it receives block k. */
    spi_bus_config_t bus = {.mosi_io_num = PIN_GPU_MOSI,
                            .miso_io_num = PIN_GPU_MISO,
                            .sclk_io_num = PIN_GPU_SCK,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1};
    spi_device_interface_config_t dev = {.clock_speed_hz = GPU_SPI_HZ,
                                         .mode = 3,
                                         .spics_io_num = PIN_GPU_CS,
                                         .queue_size = 1};
    spi_device_handle_t h;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK ||
        spi_bus_add_device(SPI2_HOST, &dev, &h) != ESP_OK) {
        printf("  cannot start SPI host A\n");
        return R_FAIL;
    }
    gpio_pulldown_en(PIN_GPU_MISO); /* open MISO reads 0x00 */

    static const uint8_t blocks[][8] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55},
        {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
        {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},
        {0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F},
        {0x48, 0x41, 0x4C, 0x54, 0x5A, 0x21, 0x0F, 0xF0}, /* "HALTZ!" */
        {0x13, 0x37, 0xC0, 0xDE, 0x9A, 0x6B, 0x2E, 0xD4},
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* only to read the last echo */
    };
    const int nblk = (int)(sizeof(blocks) / sizeof(blocks[0]));
    int good = 0, bad = 0, shown = 0;
    uint8_t all_or = 0x00, all_and = 0xFF, diff_or = 0x00;
    bool plain_echo = true; /* got the bytes back NOT inverted: MOSI tied to MISO? */
    for (int k = 0; k < nblk; k++) {
        uint8_t rx[8] = {0};
        spi_transaction_t t = {.length = 64, .tx_buffer = blocks[k], .rx_buffer = rx};
        spi_device_polling_transmit(h, &t);
        if (k > 0) {
            bool ok = true;
            for (int i = 0; i < 8; i++) {
                uint8_t want = (uint8_t)~blocks[k - 1][i];
                all_or |= rx[i];
                all_and &= rx[i];
                diff_or |= (uint8_t)(rx[i] ^ want);
                ok &= (rx[i] == want);
                plain_echo &= (rx[i] == blocks[k][i]);
            }
            if (ok) {
                good++;
            } else {
                bad++;
                if (shown++ < 3) {
                    printf("         block %d: want", k);
                    for (int i = 0; i < 8; i++) {
                        printf(" %02X", (uint8_t)~blocks[k - 1][i]);
                    }
                    printf(", got");
                    for (int i = 0; i < 8; i++) {
                        printf(" %02X", rx[i]);
                    }
                    printf("\n");
                }
            }
        }
        delay_ms(20); /* room for the Pico to queue its echo, even while it prints */
    }
    spi_bus_remove_device(h);
    spi_bus_free(SPI2_HOST);

    snprintf(what, sizeof(what), "SPI echo at %d kHz: %d/%d blocks right", GPU_SPI_HZ / 1000,
             good, good + bad);
    const char *hint = NULL;
    if (bad > 0) {
        if (all_or == 0x00) {
            hint = "MISO always 0: MISO (Pico GP19 -> S3 GPIO13) open";
        } else if (all_and == 0xFF) {
            hint = "MISO always 1: MISO shorted to 3V3, or the Pico got only 0x00 (MOSI open)";
        } else if (plain_echo) {
            hint = "the S3 reads its own data: MOSI and MISO are joined somewhere";
        } else if (diff_or != 0xFF) {
            hint = "only some bits wrong: noise. Shorter wires, or a lower GPU_SPI_HZ";
        } else {
            hint = "MOSI (S3 GPIO11 -> Pico GP16) or MISO (Pico GP19 -> S3 GPIO13) open or "
                   "swapped. The Pico console shows the bytes it received";
        }
    }
    check(&r, bad == 0, what, hint);
    gpu_pins_as_gpio();
    return r;
}

/* ======================================================================================== */
/* Stage 4: I/O link                                                                        */
/* ======================================================================================== */

static bool s_uart_ready;

static result_t stage4_io_link(void) {
    result_t r = R_PASS;
    printf("\n--- Stage 4: I/O link to the ESP32 (needs the esp32dev_wiring build on it) ---\n");

    out_pin(PIN_IO_EN, 1, true); /* released */
    if (!s_uart_ready) {
        uart_config_t cfg = {.baud_rate = IO_UART_BAUD,
                             .data_bits = UART_DATA_8_BITS,
                             .parity = UART_PARITY_DISABLE,
                             .stop_bits = UART_STOP_BITS_1,
                             .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                             .source_clk = UART_SCLK_DEFAULT};
        if (uart_driver_install(IO_UART_NUM, 1024, 0, 0, NULL, 0) != ESP_OK ||
            uart_param_config(IO_UART_NUM, &cfg) != ESP_OK ||
            uart_set_pin(IO_UART_NUM, PIN_IO_UART_TX, PIN_IO_UART_RX, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE) != ESP_OK) {
            printf("  cannot start UART1\n");
            return R_FAIL;
        }
        gpio_pullup_en(PIN_IO_UART_RX); /* open RX idles high instead of picking up noise */
        s_uart_ready = true;
    }

    /* 1. Hold the ESP32 in reset: nothing must come back. */
    gpio_set_level(PIN_IO_EN, 0);
    delay_ms(50);
    uart_flush_input(IO_UART_NUM);
    static const uint8_t probe[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x0F};
    uart_write_bytes(IO_UART_NUM, probe, sizeof(probe));
    uint8_t buf[256];
    int held = uart_read_bytes(IO_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));

    /* 2. Release: the ESP32 boots and sends its banner. */
    uart_flush_input(IO_UART_NUM);
    gpio_set_level(PIN_IO_EN, 1);
    int64_t t0 = now_ms();
    size_t got = 0;
    bool banner = false;
    while (!banner && now_ms() - t0 < ESP_BOOT_MS && got < sizeof(buf) - 1) {
        int n = uart_read_bytes(IO_UART_NUM, buf + got, sizeof(buf) - 1 - got,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            got += (size_t)n;
            buf[got] = 0;
            banner = contains(buf, got, IO_BANNER);
        }
    }
    int boot_ms = (int)(now_ms() - t0);
    delay_ms(100); /* rest of the banner line */
    uart_flush_input(IO_UART_NUM);

    /* 3. Echo: the ESP32 sends every byte back inverted. */
    uint8_t tx[64], rx[64];
    for (int i = 0; i < 64; i++) {
        static const uint8_t fixed[] = {0x00, 0xFF, 0x55, 0xAA, 0x0F, 0xF0, 0x01, 0x80};
        tx[i] = i < 8 ? fixed[i] : (i < 16 ? (uint8_t)(1u << (i - 8)) : (uint8_t)(i * 37 + 11));
    }
    uart_write_bytes(IO_UART_NUM, tx, sizeof(tx));
    int n = uart_read_bytes(IO_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(500));
    int right = 0, same = 0;
    for (int i = 0; i < n; i++) {
        right += (rx[i] == (uint8_t)~tx[i]);
        same += (rx[i] == tx[i]);
    }

    /* Verdicts */
    char what[96];
    snprintf(what, sizeof(what), "ESP32 boot banner received (%d ms after EN released)",
             boot_ms);
    const char *hint;
    if (got == 0) {
        hint = "nothing arrived: ESP32 TX2 (GPIO17) -> S3 GPIO18 open, ESP32 not running the "
               "esp32dev_wiring build, or EN (S3 GPIO8 -> ESP32 EN) not releasing";
    } else {
        hint = "bytes arrived but no banner: wrong baud, or TX2/RX2 on the wrong ESP32 pins";
        printf("         received %u bytes, first:", (unsigned)got);
        for (size_t i = 0; i < got && i < 16; i++) {
            printf(" %02X", buf[i]);
        }
        printf("\n");
    }
    check(&r, banner, what, hint);

    if (same == n && n > 0) {
        hint = "the S3 reads back its own bytes: S3 GPIO17 and GPIO18 are joined (TX not "
               "crossed to RX?)";
    } else if (n == 0) {
        hint = banner ? "no echo: S3 GPIO17 (TX) -> ESP32 RX2 (GPIO16) open"
                      : "no echo: link not working in either direction";
    } else {
        hint = "some bytes wrong: loose wire, noise, or a missing GND wire";
    }
    snprintf(what, sizeof(what), "echo S3 -> ESP32 -> S3: %d/64 bytes right", right);
    check(&r, right == 64, what, hint);

    bool en_ok = (held == 0) && banner;
    check(&r, en_ok, "EN holds the ESP32 in reset and restarts it",
          held > 0 ? "the ESP32 answered while EN was held low: EN (S3 GPIO8 -> ESP32 EN) open"
                   : "can't tell while the link fails in both directions");
    return r;
}

/* ======================================================================================== */
/* Live loop                                                                                */
/* ======================================================================================== */

void haltz_wiring_test(void) {
    printf("\n==========================================\n");
    printf(" HALTZ CPU - WIRING TEST\n");
    printf(" (wiring-v1.md stages 1, 3 and 4; S3 console at 115200)\n");
    printf("==========================================\n");

    in_pin(PIN_POWER_BUTTON, GPIO_PULLUP_ONLY);
    in_pin(PIN_HALT_BUTTON, GPIO_PULLUP_ONLY);

    result_t s1 = stage1_rc522();
    printf("  power button (GPIO16) now reads %s\n",
           gpio_get_level(PIN_POWER_BUTTON) ? "released" : "PRESSED (or shorted to GND)");
    result_t s3 = stage3_gpu_link();
    result_t s4 = stage4_io_link();

    printf("\n==========================================\n");
    printf(" Stage 1 RC522        %s\n", rname(s1));
    printf(" Stage 3 GPU link     %s\n", rname(s3));
    printf(" Stage 4 I/O link     %s\n", rname(s4));
    printf(" Stage 2 TFT and 5 I/O parts: see the Pico and ESP32 consoles.\n");
    printf("==========================================\n");
    printf("Live: press the power button, tap a card. Press BOOT to run the tests again.\n");

    int last_pwr = gpio_get_level(PIN_POWER_BUTTON);
    bool card = false;
    int misses = 0; /* a card counts as removed after 3 polls without an answer */
    uint8_t last_uid[4] = {0};
    int64_t next_poll = 0;
    while (true) {
        int pwr = gpio_get_level(PIN_POWER_BUTTON);
        if (pwr != last_pwr) {
            delay_ms(20); /* debounce */
            pwr = gpio_get_level(PIN_POWER_BUTTON);
            if (pwr != last_pwr) {
                printf("power button %s\n", pwr ? "released" : "pressed");
                last_pwr = pwr;
            }
        }

        if (gpio_get_level(PIN_HALT_BUTTON) == 0) {
            printf("BOOT pressed: release it to restart the tests\n");
            while (gpio_get_level(PIN_HALT_BUTTON) == 0) {
                delay_ms(10);
            }
            delay_ms(50);
            esp_restart();
        }

        if (s_rc_ok && now_ms() >= next_poll) {
            next_poll = now_ms() + 250;
            uint8_t uid[4];
            bool cascade = false;
            bool seen = rc_read_uid(uid, &cascade);
            if (seen) {
                misses = 0;
                if (!card || memcmp(uid, last_uid, 4) != 0) {
                    printf("card: UID %02X %02X %02X %02X%s\n", uid[0], uid[1], uid[2], uid[3],
                           cascade ? " (first part of a 7-byte UID)" : "");
                    memcpy(last_uid, uid, 4);
                }
                card = true;
            } else if (card && ++misses >= 3) {
                printf("card removed\n");
                card = false;
            }
        }
        delay_ms(10);
    }
}

#endif /* HALTZ_WIRING_TEST */
