/*
 * HALTZ I/O (ESP32 DevKit V1) - wiring test (PlatformIO env "esp32dev_wiring",
 * -DHALTZ_WIRING_TEST). Not the real I/O firmware: Bluetooth is not started.
 *
 *  Stage 4  UART2 link to the S3: sends a banner at boot, then sends every byte it receives
 *           back inverted. The S3's wiring test (env "cpu_wiring") checks it, and EN.
 *  Stage 5  Beep test on buzzer A, then B, then both. LED shows red, green, blue, white.
 *           Then live: thumbstick and knob values on the console, LED colour follows the
 *           knob (8 steps), stick button press beeps.
 */
#ifdef HALTZ_WIRING_TEST

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"

/* Same text the S3's wiring test (firmware/cpu-s3/src/wiring_test.c) looks for. */
#define IO_BANNER    "HALTZ-IO-WIRING"
#define CPU_UART_NUM UART_NUM_2
#define CPU_BAUD     115200
#define BEEP_HZ      2000 /* wiring doc: beep test at ~2 kHz */

/* ---- Stage 4: UART2 echo ---------------------------------------------------------------- */

static volatile uint32_t s_echoed;

static void uart_echo_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (true) {
        int n = uart_read_bytes(CPU_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                buf[i] = (uint8_t)~buf[i];
            }
            uart_write_bytes(CPU_UART_NUM, buf, (size_t)n);
            s_echoed += (uint32_t)n;
        }
    }
}

static void uart_link_start(void) {
    uart_config_t cfg = {.baud_rate = CPU_BAUD,
                         .data_bits = UART_DATA_8_BITS,
                         .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                         .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(CPU_UART_NUM, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CPU_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CPU_UART_NUM, PIN_CPU_UART_TX, PIN_CPU_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(PIN_CPU_UART_RX); /* open RX idles high instead of picking up noise */
    const char banner[] = IO_BANNER "\r\n";
    uart_write_bytes(CPU_UART_NUM, banner, sizeof(banner) - 1);
    xTaskCreate(uart_echo_task, "uart_echo", 3072, NULL, 10, NULL);
    printf("stage 4: banner sent on UART2 (TX2 GPIO17), echoing RX2 (GPIO16) inverted\n");
}

/* ---- Stage 5: buzzers -------------------------------------------------------------------- */

static void buzzer_release(gpio_num_t pin, ledc_channel_t ch, bool was_playing) {
    if (was_playing) {
        ledc_stop(LEDC_LOW_SPEED_MODE, ch, 0);
    }
    /* Silent = released: input, no pull-up, no pull-down (see wiring-v1.md). */
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

static void buzzer_play(gpio_num_t pin, ledc_timer_t timer, ledc_channel_t ch, uint32_t hz) {
    ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE,
                              .duty_resolution = LEDC_TIMER_10_BIT,
                              .timer_num = timer,
                              .freq_hz = hz,
                              .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = {.gpio_num = pin,
                                .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = ch,
                                .timer_sel = timer,
                                .duty = 512, /* 50% square wave */
                                .hpoint = 0};
    ledc_channel_config(&cc);
}

static void beep_a(uint32_t hz) {
    buzzer_play(PIN_BUZZER_A, LEDC_TIMER_0, LEDC_CHANNEL_0, hz);
}

static void beep_b(uint32_t hz) {
    buzzer_play(PIN_BUZZER_B, LEDC_TIMER_1, LEDC_CHANNEL_1, hz);
}

static void silence(void) {
    buzzer_release(PIN_BUZZER_A, LEDC_CHANNEL_0, true);
    buzzer_release(PIN_BUZZER_B, LEDC_CHANNEL_1, true);
}

static void beep_test(void) {
    printf("stage 5: beep test %d Hz. Clear beep = transistor on the module (keep the 1 k).\n"
           "         Barely audible = no transistor: add the NPN driver from wiring-v1.md.\n",
           BEEP_HZ);
    printf("  buzzer A (GPIO25)...\n");
    beep_a(BEEP_HZ);
    vTaskDelay(pdMS_TO_TICKS(600));
    silence();
    vTaskDelay(pdMS_TO_TICKS(400));
    printf("  buzzer B (GPIO26)...\n");
    beep_b(BEEP_HZ);
    vTaskDelay(pdMS_TO_TICKS(600));
    silence();
    vTaskDelay(pdMS_TO_TICKS(400));
    printf("  both: A 1000 Hz + B 1500 Hz (two voices)...\n");
    beep_a(1000);
    beep_b(1500);
    vTaskDelay(pdMS_TO_TICKS(800));
    silence();
}

/* ---- Stage 5: RGB LED -------------------------------------------------------------------- */

static void led_set(int rgb) { /* bit 2 = R, bit 1 = G, bit 0 = B */
    gpio_set_level(PIN_LED_R, (rgb >> 2) & 1);
    gpio_set_level(PIN_LED_G, (rgb >> 1) & 1);
    gpio_set_level(PIN_LED_B, rgb & 1);
}

static const char *const k_led_names[8] = {"off",  "blue",    "green",  "cyan",
                                           "red",  "magenta", "yellow", "white"};

static void led_test(void) {
    const gpio_num_t pins[] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
    for (size_t i = 0; i < 3; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    }
    static const int seq[] = {4, 2, 1, 7, 0};
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        printf("stage 5: LED should be %s\n", k_led_names[seq[i]]);
        led_set(seq[i]);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}

/* ---- Stage 5: thumbstick + knob ---------------------------------------------------------- */

typedef struct {
    const char *name;
    gpio_num_t pin;
    adc_channel_t ch;
} analog_t;

static adc_oneshot_unit_handle_t s_adc;

static int adc_avg(const analog_t *a, int samples, int *spread) {
    int sum = 0, lo = 4095, hi = 0;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc, a->ch, &raw);
        sum += raw;
        lo = raw < lo ? raw : lo;
        hi = raw > hi ? raw : hi;
    }
    if (spread != NULL) {
        *spread = hi - lo;
    }
    return sum / samples;
}

static void analog_init(analog_t *a, size_t n) {
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    for (size_t i = 0; i < n; i++) {
        adc_unit_t unit;
        ESP_ERROR_CHECK(adc_oneshot_io_to_channel(a[i].pin, &unit, &a[i].ch));
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, a[i].ch, &ccfg));
    }
}

/* Rest-position check: a stick axis at rest sits near the middle; an open wire floats. */
static void analog_report(const analog_t *a, bool expect_middle) {
    int spread = 0;
    int v = adc_avg(a, 32, &spread);
    const char *verdict = "OK";
    if (spread > 300) {
        verdict = "NOISY: wire open? (an unconnected input-only pin floats)";
    } else if (v > 4000) {
        verdict = "FULL SCALE: wiper shorted to 3V3, or GND of the part missing";
    } else if (v < 50) {
        verdict = "ZERO: wiper shorted to GND, or 3V3 of the part missing";
    } else if (expect_middle && (v < 900 || v > 3200)) {
        verdict = "far from the middle at rest: is the stick released?";
    }
    printf("  %-10s GPIO%-2d raw %4d (noise %3d)  %s\n", a->name, a->pin, v, spread, verdict);
}

/* ---- Entry ------------------------------------------------------------------------------- */

void haltz_io_wiring_test(void) {
    printf("\n==========================================\n");
    printf(" HALTZ IO - WIRING TEST (stages 4 and 5)\n");
    printf(" Bluetooth is off in this build.\n");
    printf("==========================================\n");

    /* Buzzers released first: silent, whatever transistor the module has. */
    buzzer_release(PIN_BUZZER_A, LEDC_CHANNEL_0, false);
    buzzer_release(PIN_BUZZER_B, LEDC_CHANNEL_1, false);

    uart_link_start(); /* first: the S3 waits for the banner right after reset */

    gpio_reset_pin(PIN_STICK_SW);
    gpio_set_direction(PIN_STICK_SW, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_STICK_SW, GPIO_PULLUP_ONLY);

    analog_t an[3] = {{"stick X", PIN_STICK_VRX, 0},
                      {"stick Y", PIN_STICK_VRY, 0},
                      {"knob", PIN_KNOB, 0}};
    analog_init(an, 3);
    printf("stage 5: analog inputs at rest (leave the stick alone):\n");
    analog_report(&an[0], true);
    analog_report(&an[1], true);
    analog_report(&an[2], false);
    printf("  stick button (GPIO32) reads %s\n",
           gpio_get_level(PIN_STICK_SW) ? "released" : "PRESSED (or shorted to GND)");
    int cx = adc_avg(&an[0], 32, NULL);
    int cy = adc_avg(&an[1], 32, NULL);

    led_test();
    beep_test();

    printf("\nLive: move the stick (X/Y relative to centre, -2048..+2047), press it (beeps),\n"
           "turn the knob end to end (0..100%%, LED colour follows it in 8 steps).\n");

    int last_x = 0, last_y = 0, last_k = -1000, last_step = -1;
    int last_sw = gpio_get_level(PIN_STICK_SW);
    uint32_t last_echoed = 0;
    TickType_t last_echo_report = xTaskGetTickCount();
    while (true) {
        int x = adc_avg(&an[0], 8, NULL) - cx;
        int y = adc_avg(&an[1], 8, NULL) - cy;
        if (STICK_SWAP_XY) {
            int t = x;
            x = y;
            y = t;
        }
        if (STICK_INVERT_X) {
            x = -x;
        }
        if (STICK_INVERT_Y) {
            y = -y;
        }
        int k = adc_avg(&an[2], 16, NULL);
        int sw = gpio_get_level(PIN_STICK_SW);

        if (abs(x - last_x) > 80 || abs(y - last_y) > 80 || abs(k - last_k) > 40) {
            printf("stick X %+5d  Y %+5d   knob %3d%% (raw %4d)\n", x, y, k * 100 / 4095, k);
            last_x = x;
            last_y = y;
            last_k = k;
        }
        int step = k * 8 / 4096;
        if (step != last_step) {
            led_set(step);
            printf("LED %s\n", k_led_names[step]);
            last_step = step;
        }
        if (sw != last_sw) {
            vTaskDelay(pdMS_TO_TICKS(20)); /* debounce */
            sw = gpio_get_level(PIN_STICK_SW);
            if (sw != last_sw) {
                printf("stick button %s\n", sw ? "released" : "pressed");
                if (!sw) {
                    beep_a(BEEP_HZ);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    silence();
                    beep_b(BEEP_HZ * 3 / 2);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    silence();
                }
                last_sw = sw;
            }
        }
        if (xTaskGetTickCount() - last_echo_report > pdMS_TO_TICKS(1000)) {
            uint32_t e = s_echoed;
            if (e != last_echoed) {
                printf("UART2: echoed %lu bytes from the S3 (total %lu)\n",
                       (unsigned long)(e - last_echoed), (unsigned long)e);
                last_echoed = e;
            }
            last_echo_report = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#endif /* HALTZ_WIRING_TEST */
