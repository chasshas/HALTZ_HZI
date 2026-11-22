/*
 * HALTZ I/O: thumbstick, knob and buttons. See io_input.h.
 *
 * Stick: 4 readings per axis every 10 ms, a light filter (half of each new sample), then the
 * board_pins.h orientation fix, a dead zone, and scaling to -1000..1000 separately on each
 * side of the centre. Knob: 8 readings, a stronger filter, the flat ADC ends trimmed, 0-255
 * with a 2-step change threshold. Buttons: two equal samples in a row (20 ms debounce).
 */
#include "io_input.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#include "board_pins.h"

#define ADC_FULL       4095
#define END_TRIM       40   /* the ADC is flat at the very ends: full deflection reads this */
#define DEAD_ZONE      110  /* raw counts around the centre that read 0 (~2.7% of the range) */
#define CENTRE_MIN     900  /* a released stick reads near mid-scale; outside: wiring fault */
#define CENTRE_MAX     3200
#define CAL_SAMPLES    32
#define STICK_SAMPLES  4
#define STICK_HYST     4    /* report an axis change of at least this much (of 1000) */
#define KNOB_SAMPLES   8
#define KNOB_LO        60   /* raw value that reads 0 ... */
#define KNOB_HI        4035 /* ... and 255 */
#define KNOB_HYST      2

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_ch_x, s_ch_y, s_ch_knob;
static input_state_t s_st;
static int s_fx16, s_fy16, s_fk16; /* filtered raw values x 16 */
static bool s_filters_primed;
static uint8_t s_btn_last_raw, s_btn_stable;

static int read_avg(adc_channel_t ch, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc, ch, &raw);
        sum += raw;
    }
    return sum / n;
}

static uint8_t read_buttons(void) {
    uint8_t b = 0;
    if (gpio_get_level(PIN_STICK_SW) == 0) {
        b |= HALTZ_IO_BTN_STICK;
    }
    if (gpio_get_level(PIN_SPARE_BUTTON) == 0) {
        b |= HALTZ_IO_BTN_SPARE;
    }
    return b;
}

/* Offset from the centre (raw counts, orientation already fixed) -> -1000..1000. */
static int16_t axis_value(int off, int span_pos, int span_neg) {
    if (abs(off) <= DEAD_ZONE) {
        return 0;
    }
    int span = off > 0 ? span_pos : span_neg;
    if (span < 1) {
        span = 1;
    }
    int v = (abs(off) - DEAD_ZONE) * HALTZ_IO_AXIS_MAX / span;
    if (v > HALTZ_IO_AXIS_MAX) {
        v = HALTZ_IO_AXIS_MAX;
    }
    return (int16_t)(off > 0 ? v : -v);
}

/* Each side of the centre is scaled on its own: from the dead zone edge to the trimmed end. */
static void stick_values(int fx, int fy, int16_t *x, int16_t *y) {
    int cx = s_st.centre_x, cy = s_st.centre_y;
    int ox = fx - cx, oy = fy - cy;
    /* Room above and below the centre on each raw axis. */
    int up_x = (ADC_FULL - END_TRIM) - cx - DEAD_ZONE, down_x = cx - END_TRIM - DEAD_ZONE;
    int up_y = (ADC_FULL - END_TRIM) - cy - DEAD_ZONE, down_y = cy - END_TRIM - DEAD_ZONE;
    int16_t vx = axis_value(ox, up_x, down_x);
    int16_t vy = axis_value(oy, up_y, down_y);
    if (STICK_SWAP_XY) {
        int16_t t = vx;
        vx = vy;
        vy = t;
    }
    if (STICK_INVERT_X) {
        vx = (int16_t)-vx;
    }
    if (STICK_INVERT_Y) {
        vy = (int16_t)-vy;
    }
    *x = vx;
    *y = vy;
}

void input_calibrate(void) {
    int cx = read_avg(s_ch_x, CAL_SAMPLES);
    int cy = read_avg(s_ch_y, CAL_SAMPLES);
    s_st.stick_ok = cx >= CENTRE_MIN && cx <= CENTRE_MAX && cy >= CENTRE_MIN && cy <= CENTRE_MAX;
    s_st.centre_x = s_st.stick_ok ? cx : ADC_FULL / 2;
    s_st.centre_y = s_st.stick_ok ? cy : ADC_FULL / 2;
    s_fx16 = cx * 16;
    s_fy16 = cy * 16;
}

esp_err_t input_start(void) {
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    const int pins[3] = {PIN_STICK_VRX, PIN_STICK_VRY, PIN_KNOB};
    adc_channel_t *chs[3] = {&s_ch_x, &s_ch_y, &s_ch_knob};
    for (int i = 0; i < 3; i++) {
        adc_unit_t unit;
        err = adc_oneshot_io_to_channel(pins[i], &unit, chs[i]);
        if (err == ESP_OK) {
            err = adc_oneshot_config_channel(s_adc, *chs[i], &ccfg);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    const gpio_num_t buttons[2] = {PIN_STICK_SW, PIN_SPARE_BUTTON};
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(buttons[i]);
        gpio_set_direction(buttons[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(buttons[i], GPIO_PULLUP_ONLY);
    }
    s_btn_last_raw = s_btn_stable = read_buttons();
    s_st.stick.buttons = s_btn_stable;

    input_calibrate();
    s_fk16 = read_avg(s_ch_knob, KNOB_SAMPLES) * 16;
    s_filters_primed = true;
    bool sc, kc;
    input_sample(&sc, &kc); /* first values; the link sends them with its first refresh */
    return ESP_OK;
}

void input_sample(bool *stick_changed, bool *knob_changed) {
    *stick_changed = false;
    *knob_changed = false;
    if (!s_filters_primed) {
        return;
    }

    int rx = read_avg(s_ch_x, STICK_SAMPLES);
    int ry = read_avg(s_ch_y, STICK_SAMPLES);
    int rk = read_avg(s_ch_knob, KNOB_SAMPLES);
    s_fx16 += (rx * 16 - s_fx16) / 2;
    s_fy16 += (ry * 16 - s_fy16) / 2;
    s_fk16 += (rk * 16 - s_fk16) / 4;
    s_st.raw_x = s_fx16 / 16;
    s_st.raw_y = s_fy16 / 16;
    s_st.raw_knob = s_fk16 / 16;

    /* Buttons: debounced over two samples. */
    uint8_t b = read_buttons();
    if (b == s_btn_last_raw) {
        s_btn_stable = b;
    }
    s_btn_last_raw = b;

    int16_t x = 0, y = 0;
    if (s_st.stick_ok) {
        stick_values(s_st.raw_x, s_st.raw_y, &x, &y);
    }
    haltz_io_stick_t *st = &s_st.stick;
    bool moved = abs(x - st->x) >= STICK_HYST || abs(y - st->y) >= STICK_HYST ||
                 ((x == 0) != (st->x == 0)) || ((y == 0) != (st->y == 0)) ||
                 ((abs(x) == HALTZ_IO_AXIS_MAX) != (abs(st->x) == HALTZ_IO_AXIS_MAX)) ||
                 ((abs(y) == HALTZ_IO_AXIS_MAX) != (abs(st->y) == HALTZ_IO_AXIS_MAX));
    if (moved || s_btn_stable != st->buttons) {
        st->x = x;
        st->y = y;
        st->buttons = s_btn_stable;
        *stick_changed = true;
    }

    int k = (s_st.raw_knob - KNOB_LO) * 255 / (KNOB_HI - KNOB_LO);
    k = k < 0 ? 0 : (k > 255 ? 255 : k);
    if (abs(k - s_st.knob) >= KNOB_HYST || ((k == 0 || k == 255) && k != s_st.knob)) {
        s_st.knob = (uint8_t)k;
        *knob_changed = true;
    }
}

const input_state_t *input_state(void) {
    return &s_st;
}
