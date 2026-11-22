/*
 * HALTZ I/O: buzzers and status LED on LEDC. See io_fx.h.
 *
 * LEDC (low-speed mode, APB clock, 10-bit duty):
 *   timer 0 / channel 0  buzzer A (GPIO25), frequency changes per note
 *   timer 1 / channel 1  buzzer B (GPIO26)
 *   timer 2 / channels 2-4  LED R, G, B (GPIO18, 19, 21), 5 kHz
 * A silent buzzer has its pin released (input, no pull: wiring-v1.md). Playing = square wave;
 * volume = pulse width (50% at full volume).
 */
#include "io_fx.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "ledfx.h"
#include "tunes.h"

#define DUTY_BITS    LEDC_TIMER_10_BIT
#define DUTY_MAX     1023u
#define LED_HZ       5000
#define LED_EVERY_MS 10
#define TASK_STACK   3072
#define TASK_PRIO    7 /* above io_link: note timing stays exact */
#define TASK_CORE    1 /* architecture-v1: Bluetooth on core 0, sound/LED/ADC on core 1 */

typedef struct {
    gpio_num_t pin;
    ledc_timer_t timer;
    ledc_channel_t ch;
} buzzer_t;

static const buzzer_t k_buzzer[HALTZ_IO_VOICES] = {
    {PIN_BUZZER_A, LEDC_TIMER_0, LEDC_CHANNEL_0},
    {PIN_BUZZER_B, LEDC_TIMER_1, LEDC_CHANNEL_1},
};
static const gpio_num_t k_led_pin[3] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
static const ledc_channel_t k_led_ch[3] = {LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4};

static SemaphoreHandle_t s_lock;
static seq_t s_seq;   /* under s_lock */
static ledfx_t s_led; /* under s_lock */
static uint8_t s_volume[HALTZ_IO_VOICES] = {HALTZ_IO_VOLUME_FULL, HALTZ_IO_VOLUME_FULL};
static uint16_t s_gamma[256]; /* 8-bit colour -> 10-bit duty */

/* What the hardware does now (fx task only). */
static uint16_t s_out_hz[HALTZ_IO_VOICES];
static uint32_t s_out_duty[HALTZ_IO_VOICES];

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Volume 0..255 -> pulse width, 0..50%. Square curve: the ear hears the change of a narrow
 * pulse much more than that of a wide one. */
static uint32_t volume_duty(uint8_t level) {
    if (level == 0) {
        return 0;
    }
    uint32_t d = (uint32_t)level * level * (DUTY_MAX / 2 + 1) / (255u * 255u);
    return d < 1 ? 1 : d;
}

static void buzzer_release(int v) {
    ledc_stop(LEDC_LOW_SPEED_MODE, k_buzzer[v].ch, 0);
    gpio_set_direction(k_buzzer[v].pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(k_buzzer[v].pin, GPIO_FLOATING);
}

/* Bring the buzzer output to (hz, duty); hz 0 or duty 0 = released. */
static void buzzer_apply(int v, uint16_t hz, uint32_t duty) {
    if (hz == 0 || duty == 0) {
        hz = 0;
        duty = 0;
    }
    if (hz == s_out_hz[v] && duty == s_out_duty[v]) {
        return;
    }
    const buzzer_t *b = &k_buzzer[v];
    if (hz == 0) {
        buzzer_release(v);
    } else if (s_out_hz[v] == 0) { /* from silence: attach the pin to LEDC again */
        ledc_set_freq(LEDC_LOW_SPEED_MODE, b->timer, hz);
        ledc_channel_config_t cc = {.gpio_num = b->pin,
                                    .speed_mode = LEDC_LOW_SPEED_MODE,
                                    .channel = b->ch,
                                    .timer_sel = b->timer,
                                    .duty = duty,
                                    .hpoint = 0};
        ledc_channel_config(&cc);
    } else {
        if (hz != s_out_hz[v]) {
            ledc_set_freq(LEDC_LOW_SPEED_MODE, b->timer, hz);
        }
        if (duty != s_out_duty[v]) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, b->ch, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, b->ch);
        }
    }
    s_out_hz[v] = hz;
    s_out_duty[v] = duty;
}

static void led_apply(rgb_t c) {
    const uint8_t v[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; i++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, k_led_ch[i], s_gamma[v[i]]);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, k_led_ch[i]);
    }
}

static void fx_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    uint32_t last = now_ms();
    uint32_t next_led = last;
    for (;;) {
        vTaskDelayUntil(&wake, 1); /* CONFIG_FREERTOS_HZ = 1000: one tick = 1 ms */
        uint32_t now = now_ms();
        uint16_t hz[HALTZ_IO_VOICES];
        uint32_t duty[HALTZ_IO_VOICES];
        bool led_due = (int32_t)(now - next_led) >= 0;
        rgb_t c = {0, 0, 0};

        xSemaphoreTake(s_lock, portMAX_DELAY);
        seq_advance(&s_seq, now - last);
        for (int v = 0; v < HALTZ_IO_VOICES; v++) {
            hz[v] = seq_output_hz(&s_seq, v);
            duty[v] = volume_duty(s_volume[v]);
        }
        if (led_due) {
            c = ledfx_eval(&s_led, now);
        }
        xSemaphoreGive(s_lock);
        last = now;

        for (int v = 0; v < HALTZ_IO_VOICES; v++) {
            buzzer_apply(v, hz[v], duty[v]);
        }
        if (led_due) {
            led_apply(c);
            next_led = now + LED_EVERY_MS;
        }
    }
}

haltz_err_t fx_cmd(const haltz_io_cmd_t *cmd) {
    haltz_err_t err = HALTZ_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (cmd->type) {
    case HALTZ_MSG_IO_LED:
        ledfx_set(&s_led, &cmd->u.led, now_ms());
        break;
    case HALTZ_MSG_IO_TONE:
        seq_tone(&s_seq, cmd->u.tone.voice, cmd->u.tone.hz, cmd->u.tone.ms);
        break;
    case HALTZ_MSG_IO_NOTES:
        err = seq_notes(&s_seq, &cmd->u.notes);
        break;
    case HALTZ_MSG_IO_SFX:
        seq_sfx(&s_seq, cmd->u.sfx.voice, cmd->u.sfx.id);
        break;
    case HALTZ_MSG_IO_STOP:
        for (int v = 0; v < HALTZ_IO_VOICES; v++) {
            if (cmd->u.stop.voices & (1u << v)) {
                seq_stop(&s_seq, v);
            }
        }
        break;
    case HALTZ_MSG_IO_VOLUME:
        for (int v = 0; v < HALTZ_IO_VOICES; v++) {
            if (cmd->u.volume.voices & (1u << v)) {
                s_volume[v] = cmd->u.volume.level;
            }
        }
        break;
    default:
        err = HALTZ_ERR_UNKNOWN_TYPE;
        break;
    }
    xSemaphoreGive(s_lock);
    return err;
}

uint8_t fx_status(uint8_t tune_free[2]) {
    uint8_t flags = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int v = 0; v < HALTZ_IO_VOICES; v++) {
        tune_free[v] = seq_free(&s_seq, v);
        if (seq_output_hz(&s_seq, v) != 0 && s_volume[v] != 0) {
            flags |= (v == 0) ? HALTZ_IO_STATUS_VOICE_A : HALTZ_IO_STATUS_VOICE_B;
        }
    }
    xSemaphoreGive(s_lock);
    return flags;
}

esp_err_t fx_start(void) {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    seq_init(&s_seq);
    ledfx_init(&s_led);
    for (int i = 0; i < 256; i++) {
        s_gamma[i] = (uint16_t)lroundf(powf(i / 255.0f, 2.2f) * DUTY_MAX);
    }

    /* Buzzers: silent first, whatever transistor the module has. */
    for (int v = 0; v < HALTZ_IO_VOICES; v++) {
        ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                  .duty_resolution = DUTY_BITS,
                                  .timer_num = k_buzzer[v].timer,
                                  .freq_hz = 1000,
                                  .clk_cfg = LEDC_USE_APB_CLK}; /* fixed clock: 1 Hz .. 78 kHz */
        esp_err_t err = ledc_timer_config(&tc);
        if (err != ESP_OK) {
            return err;
        }
        gpio_reset_pin(k_buzzer[v].pin);
        gpio_set_direction(k_buzzer[v].pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(k_buzzer[v].pin, GPIO_FLOATING);
    }

    ledc_timer_config_t lt = {.speed_mode = LEDC_LOW_SPEED_MODE,
                              .duty_resolution = DUTY_BITS,
                              .timer_num = LEDC_TIMER_2,
                              .freq_hz = LED_HZ,
                              .clk_cfg = LEDC_USE_APB_CLK};
    esp_err_t err = ledc_timer_config(&lt);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t cc = {.gpio_num = k_led_pin[i],
                                    .speed_mode = LEDC_LOW_SPEED_MODE,
                                    .channel = k_led_ch[i],
                                    .timer_sel = LEDC_TIMER_2,
                                    .duty = 0,
                                    .hpoint = 0};
        err = ledc_channel_config(&cc);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Booting: breathe blue until the S3 says otherwise (protocol section 5). */
    haltz_io_led_t boot = {.mode = HALTZ_IO_LED_BREATHE, .b = 255, .period_ms = 2000};
    ledfx_set(&s_led, &boot, now_ms());

    if (xTaskCreatePinnedToCore(fx_task, "io_fx", TASK_STACK, NULL, TASK_PRIO, NULL,
                                TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
