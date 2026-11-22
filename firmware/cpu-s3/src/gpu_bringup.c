/*
 * HALTZ CPU: bring-up screen. GPU link statistics and stress test (docs/gpu-protocol-v1.md
 * section 9), I/O link state, input and sound (io_bringup.c, docs/io-protocol-v1.md
 * section 11). Stands in for the kernel until there is one.
 *
 * Screen: row 0 title, rows 1-3 GPU link statistics (twice a second), rows 4-5 the I/O link
 * and input, row 6 a separator, rows 7-28 the console window (everything printed and every
 * key typed on the Bluetooth keyboard: console.c), row 29 the keys. The stick moves the
 * pointer.
 * Keys on either S3 USB port: the "UART" port (UART0, the primary console, 115200 baud) or
 * the native "USB" port (USB Serial/JTAG; ESP-IDF makes it an output-only secondary console,
 * so its keys are read here through the USB Serial/JTAG driver):
 *   s  print the link statistics
 *   f  switch the SPI clock between 4 MHz and 10 MHz
 *   r  hard-reset the GPU (the picture must come back by itself)
 *   t  start / stop the stress test: random valid commands of every kind, a PRESENT every
 *      16 ms. When it stops it checks crc_errors, rx_overruns and NAKs stayed 0, no reset
 *      happened, and every PRESENT got its FRAME pulse.
 *   g  the demo scene (demo_scene.c, protocol section 9 step 5): every layer, driven from
 *      here, with the frame rate measured on the S3. Any key goes back.
 *   i p b l c e k  the I/O link keys (io_bringup.c)
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "console.h"
#include "demo_scene.h"
#include "gpu.h"
#include "haltz/msgs.h"
#include "io_bringup.h"

#define UPDATE_MS        500
#define STRESS_FRAME_MS  16
#define STRESS_REPORT_MS 1000

/* Layout */
#define ROW_STATS   1 /* 3 rows */
#define ROW_IO      4 /* 2 rows */
#define ROW_SEP     6
#define ROW_LOG     7
#define LOG_ROWS    22
#define ROW_KEYS    29

enum { C_TEXT = HALTZ_COLOR_WHITE, C_DIM = HALTZ_COLOR_LIGHT_GREY, C_BAD = HALTZ_COLOR_RED };

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* Print padded to the end of the row, so a shorter value overwrites a longer old one. */
static void put_line(int row, uint8_t fg, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void put_line(int row, uint8_t fg, const char *fmt, ...) {
    char buf[HALTZ_GPU_TEXT_COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n < HALTZ_GPU_TEXT_COLS) {
        buf[n++] = ' ';
    }
    gpu_text_put(0, (uint8_t)row, fg, 0, (const uint8_t *)buf, HALTZ_GPU_TEXT_COLS);
}

/* ---- status screen ----------------------------------------------------------------------- */

/* Everything outside the console window. Doesn't clear the screen (the window keeps its
 * log); after a GPU_INIT call con_redraw() too. */
static void draw_screen(void) {
    gpu_text_fill(0, 0, HALTZ_GPU_TEXT_COLS, 1, ' ', C_TEXT, HALTZ_COLOR_DARK_BLUE);
    gpu_text_print(1, 0, C_TEXT, HALTZ_COLOR_DARK_BLUE, "HALTZ CPU - bring-up");
    uint8_t sep[HALTZ_GPU_TEXT_COLS];
    memset(sep, HALTZ_GLYPH_BOX_H, sizeof(sep));
    gpu_text_put(0, ROW_SEP, HALTZ_COLOR_DARK_GREY, 0, sep, sizeof(sep));
    gpu_text_print(2, ROW_SEP, HALTZ_COLOR_YELLOW, 0, " log ");
    gpu_text_fill(0, ROW_KEYS, HALTZ_GPU_TEXT_COLS, 1, ' ', C_DIM, HALTZ_COLOR_DARK_BLUE);
    gpu_text_print(1, ROW_KEYS, C_DIM, HALTZ_COLOR_DARK_BLUE,
                   "GPU s f r t g  IO i p b l c e k");
}

static void update_screen(void) {
    gpu_stats_t st;
    gpu_get_stats(&st);
    const haltz_gpu_status_t *g = &st.status;
    bool mismatch = st.peer_proto != HALTZ_PROTO_VERSION;
    put_line(ROW_STATS, mismatch ? C_BAD : C_TEXT, " GPU v%u %s  %" PRIu32 " kHz  %s%s",
             (unsigned)st.peer_proto, st.peer_git, st.spi_hz / 1000,
             gpu_link_state_name(st.state), mismatch ? " DIFFERS" : "");
    put_line(ROW_STATS + 1, C_TEXT, " frames %" PRIu32 "/%" PRIu32 "  %u us  free %uK", st.frames,
             st.presents, g->render_us, g->rx_free_kb);
    bool bad = g->crc_errors || g->rx_overruns || st.naks || st.miso_crc_errors;
    put_line(ROW_STATS + 2, bad ? C_BAD : C_DIM,
             " crc %u ovr %u nak %" PRIu32 " miso %" PRIu32 " rst %" PRIu32, g->crc_errors,
             g->rx_overruns, st.naks, st.miso_crc_errors, st.resets);
    io_bringup_draw();
}

/* Short lines: they also go to the 40-column console window. */
static void print_stats(void) {
    gpu_stats_t st;
    gpu_get_stats(&st);
    printf("link %s, %" PRIu32 " Hz\n", gpu_link_state_name(st.state), st.spi_hz);
    printf(" GPU protocol v%u, git %s\n", (unsigned)st.peer_proto, st.peer_git);
    printf(" resets %" PRIu32 ", link ups %" PRIu32 "\n", st.resets, st.link_ups);
    printf(" last reset: %s\n", st.last_reset_reason ? st.last_reset_reason : "-");
    printf(" PRESENTs %" PRIu32 ", FRAME pulses %" PRIu32 "\n", st.presents, st.frames);
    printf(" NAKs %" PRIu32 ", MISO crc errors %" PRIu32 "\n", st.naks, st.miso_crc_errors);
    printf(" BUSY waits %" PRIu32 ", %" PRIu32 " transfers\n", st.busy_waits, st.transactions);
    printf(" %" PRIu64 " KB sent, console dropped %" PRIu32 "\n", st.tx_bytes / 1024,
           con_dropped());
    if (st.have_status) {
        const haltz_gpu_status_t *g = &st.status;
        printf("GPU_STATUS (%" PRIu32 " ms old)\n", st.status_age_ms);
        printf(" present_id %u, frames %u\n", g->present_id, g->frames);
        printf(" render %u us, rx free %u KB, flags %02X\n", g->render_us, g->rx_free_kb,
               g->flags);
        printf(" crc %u, NAKs %u, overruns %u\n", g->crc_errors, g->cmd_errors, g->rx_overruns);
        printf(" uptime %" PRIu32 " ms\n", g->uptime_ms);
    } else {
        printf("no GPU_STATUS yet\n");
    }
}

/* ---- other pictures take the screen ----------------------------------------------------- */

/* While the stress test or the demo own the screen, the console keeps recording (con_hold);
 * afterwards the bring-up screen and the log come back. */
static void screen_take(void) {
    con_hold(true);
}

static void screen_give_back(void) {
    gpu_init_picture();
    io_bringup_redraw(); /* the pointer pattern */
    draw_screen();
    update_screen();
    con_hold(false); /* redraws the log */
    gpu_present();
}

/* ---- stress test ------------------------------------------------------------------------- */

static uint32_t s_rng = 1;

static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int rr(int lo, int hi) {
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static void random_pattern(uint8_t index) {
    uint8_t d[HALTZ_GPU_PATTERN_BYTES];
    for (int i = 0; i < HALTZ_GPU_PATTERN_BYTES; i++) {
        d[i] = (rnd() & 3) ? (uint8_t)rnd() : 0; /* some holes */
    }
    gpu_pattern_load(index, d);
}

/* One random command, always valid (so the GPU must never NAK), often partly off screen. */
static void random_command(void) {
    uint8_t buf[HALTZ_GPU_BMP_PIXELS_MAX + 40];
    switch (rnd() % 16) {
    case 0:
    case 1: {
        int n = rr(1, 60);
        for (int i = 0; i < n; i++) {
            buf[i] = (uint8_t)rr(0x20, 0x92);
        }
        gpu_text_put((uint8_t)rr(0, 39), (uint8_t)rr(0, 29), (uint8_t)rr(1, 255),
                     (uint8_t)((rnd() & 1) ? 0 : rr(1, 255)), buf, (size_t)n);
        break;
    }
    case 2:
        gpu_text_fill((uint8_t)rr(0, 45), (uint8_t)rr(0, 35), (uint8_t)rr(0, 12),
                      (uint8_t)rr(0, 6), (uint8_t)rr(0x20, 0x92), (uint8_t)rr(1, 255),
                      (uint8_t)rr(0, 255));
        break;
    case 3:
        gpu_text_scroll((uint8_t)rr(0, 31), (uint8_t)rr(0, 31), (int8_t)rr(-6, 6),
                        (uint8_t)rr(0x20, 0x7E), (uint8_t)rr(1, 255), (uint8_t)rr(0, 255));
        break;
    case 4:
        gpu_text_cursor((uint8_t)rr(0, 44), (uint8_t)rr(0, 34),
                        (uint8_t)(rr(0, 2) | ((rnd() & 1) ? HALTZ_GPU_CURSOR_BLINK : 0)));
        break;
    case 5:
    case 6:
    case 7:
        gpu_sprite_set((uint8_t)rr(0, 63), (int16_t)rr(-20, 340), (int16_t)rr(-20, 260),
                       (uint8_t)rr(0, 15), (uint8_t)rnd());
        break;
    case 8: {
        int n = rr(1, 40);
        uint16_t e[40];
        for (int i = 0; i < n; i++) {
            e[i] = (uint16_t)(rnd() & 0x3F0Fu); /* pattern 0-15, any bank, flips */
        }
        gpu_tile_put((uint8_t)rr(0, 31), (uint8_t)rr(0, 31), e, (size_t)n);
        break;
    }
    case 9:
        gpu_tile_fill((uint8_t)rr(0, 35), (uint8_t)rr(0, 35), (uint8_t)rr(0, 8),
                      (uint8_t)rr(0, 8), (uint16_t)(rnd() & 0x3F0Fu));
        break;
    case 10:
        gpu_tile_scroll((uint16_t)rnd(), (uint16_t)rnd());
        break;
    case 11: {
        int n = rr(1, HALTZ_GPU_BMP_PIXELS_MAX + 30); /* sometimes 2 messages */
        for (int i = 0; i < n; i++) {
            buf[i] = (uint8_t)rnd();
        }
        gpu_bmp_pixels((uint16_t)rr(0, 330), (uint16_t)rr(0, 245), buf, (size_t)n);
        break;
    }
    case 12:
        gpu_bmp_fill((uint16_t)rr(0, 330), (uint16_t)rr(0, 245), (uint16_t)rr(0, 60),
                     (uint16_t)rr(0, 60), (uint8_t)rnd());
        break;
    case 13:
        gpu_bmp_line((int16_t)rr(-60, 380), (int16_t)rr(-60, 300), (int16_t)rr(-60, 380),
                     (int16_t)rr(-60, 300), (uint8_t)rnd());
        break;
    case 14:
        gpu_bmp_copy((uint16_t)rr(0, 330), (uint16_t)rr(0, 245), (uint16_t)rr(0, 64),
                     (uint16_t)rr(0, 64), (int16_t)rr(-40, 350), (int16_t)rr(-40, 270));
        break;
    case 15:
        random_pattern((uint8_t)rr(0, 15));
        break;
    }
}

typedef struct {
    bool on;
    gpu_stats_t start;
    int64_t next_frame_ms, next_report_ms, start_ms;
    uint32_t frames_done;
} stress_t;

static void stress_begin(stress_t *t) {
    s_rng = esp_random() | 1u;
    printf("stress: starting, any key stops it\n");
    printf(" random commands, PRESENT every %d ms\n", STRESS_FRAME_MS);
    screen_take();
    gpu_init_picture();
    gpu_layers(HALTZ_GPU_LAYERS_ALL);
    for (int i = 0; i < 16; i++) {
        random_pattern((uint8_t)i);
    }
    gpu_present();
    gpu_vsync(500);
    vTaskDelay(pdMS_TO_TICKS(600)); /* a fresh GPU_STATUS for the baseline */
    gpu_get_stats(&t->start);
    t->on = true;
    t->start_ms = now_ms();
    t->next_frame_ms = t->start_ms;
    t->next_report_ms = t->start_ms + STRESS_REPORT_MS;
    t->frames_done = 0;
}

static void stress_step(stress_t *t) {
    int64_t now = now_ms();
    if (now < t->next_frame_ms) {
        return;
    }
    t->next_frame_ms += STRESS_FRAME_MS;
    if (t->next_frame_ms < now) {
        t->next_frame_ms = now; /* the GPU was slower than 16 ms: don't try to catch up */
    }
    int n = rr(10, 100);
    for (int i = 0; i < n; i++) {
        random_command();
    }
    if (rr(0, 99) == 0) { /* rarely: things that redraw the whole screen */
        uint16_t pal[64];
        for (int i = 0; i < 64; i++) {
            pal[i] = (uint16_t)rnd();
        }
        gpu_palette((uint8_t)rr(1, 192), pal, 64);
    } else if (rr(0, 99) == 0) {
        gpu_layers((uint8_t)rr(1, HALTZ_GPU_LAYERS_ALL));
    }
    gpu_present();
    t->frames_done++;

    if (now >= t->next_report_ms) {
        t->next_report_ms += STRESS_REPORT_MS;
        gpu_stats_t st;
        gpu_get_stats(&st);
        uint32_t secs = (uint32_t)((now - t->start_ms) / 1000);
        printf("stress %" PRIu32 "s %" PRIu32 "fps %" PRIu64 "KB crc %u ovr %u nak %u rst %" PRIu32
               "\n",
               secs, secs ? t->frames_done / secs : 0, (st.tx_bytes - t->start.tx_bytes) / 1024,
               (unsigned)(uint16_t)(st.status.crc_errors - t->start.status.crc_errors),
               (unsigned)(uint16_t)(st.status.rx_overruns - t->start.status.rx_overruns),
               (unsigned)(st.naks - t->start.naks), st.resets - t->start.resets);
    }
}

static void stress_end(stress_t *t) {
    t->on = false;
    gpu_vsync(1000);
    vTaskDelay(pdMS_TO_TICKS(600)); /* a GPU_STATUS from after the last frame */
    gpu_stats_t st;
    gpu_get_stats(&st);
    uint16_t crc = (uint16_t)(st.status.crc_errors - t->start.status.crc_errors);
    uint16_t ovr = (uint16_t)(st.status.rx_overruns - t->start.status.rx_overruns);
    uint16_t gnak = (uint16_t)(st.status.cmd_errors - t->start.status.cmd_errors);
    uint32_t resets = st.resets - t->start.resets;
    uint32_t miso = st.miso_crc_errors - t->start.miso_crc_errors;
    bool frames_ok = st.frames == st.presents;
    bool pass = st.have_status && crc == 0 && ovr == 0 && gnak == 0 && resets == 0 &&
                miso == 0 && frames_ok;
    uint32_t secs = (uint32_t)((now_ms() - t->start_ms) / 1000);
    printf("stress: stopped after %" PRIu32 " s\n", secs);
    printf(" %" PRIu32 " frames, %" PRIu64 " KB at %" PRIu32 " kHz\n", t->frames_done,
           (st.tx_bytes - t->start.tx_bytes) / 1024, st.spi_hz / 1000);
    printf(" GPU crc %u, overruns %u, NAKs %u\n", crc, ovr, gnak);
    printf(" MISO crc %" PRIu32 ", resets %" PRIu32 "\n", miso, resets);
    printf(" PRESENTs %" PRIu32 " / FRAMEs %" PRIu32 "\n", st.presents, st.frames);
    printf("stress: %s\n", pass ? "PASS" : "FAIL");
    screen_give_back();
}

/* ---- demo -------------------------------------------------------------------------------- */

static int read_key(void);

static void demo_emit(const haltz_gpu_cmd_t *cmd, void *ctx) {
    if (gpu_cmd(cmd) != HALTZ_OK) {
        (*(uint32_t *)ctx)++; /* a bug in the scene: the driver refused the values */
    }
}

static void demo_run(void) {
    printf("demo: any key goes back\n");
    screen_take();
    uint32_t refused = 0;
    gpu_stats_t st0;
    gpu_get_stats(&st0);
    demo_scene_init(demo_emit, &refused);

    int64_t start = now_ms(), next_second = start + 1000;
    uint32_t frame = 0, this_second = 0;
    int fps = -1;
    while (read_key() == EOF) {
        demo_scene_step(++frame, fps, demo_emit, &refused);
        fps = -1;
        gpu_present(); /* waits while the previous frame is on its way: that is the pace */
        this_second++;
        if (now_ms() >= next_second) {
            fps = (int)this_second; /* shown with the next frame */
            this_second = 0;
            next_second += 1000;
        }
        vTaskDelay(1);
    }
    gpu_vsync(500);
    gpu_stats_t st;
    gpu_get_stats(&st);
    uint32_t ms = (uint32_t)(now_ms() - start);
    printf("demo: %" PRIu32 " frames in %" PRIu32 ".%" PRIu32 " s\n", frame, ms / 1000,
           ms % 1000 / 100);
    printf(" %" PRIu32 " fps, %" PRIu64 " KB sent\n", ms ? frame * 1000u / ms : 0,
           (st.tx_bytes - st0.tx_bytes) / 1024);
    printf(" refused %" PRIu32 ", NAKs %" PRIu32 ", resets %" PRIu32 "\n", refused,
           st.naks - st0.naks, st.resets - st0.resets);
    screen_give_back();
}

/* ---- main loop --------------------------------------------------------------------------- */

static bool s_usb_keys;

/* Native USB port: install its driver so we can read from it, and let the console's output
 * on that port go through the driver too (two writers on its FIFO would garble it). With no
 * terminal on that port, output to it is dropped after one 50 ms try, nothing stalls. */
static void keys_init(void) {
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        s_usb_keys = true;
    } else {
        printf("keys: no USB Serial/JTAG driver (%s): keys only on the UART port\n",
               esp_err_to_name(err));
    }
#endif
}

static int read_key(void) {
    int c = getchar(); /* UART0 console: returns EOF at once when no key is waiting */
    if (c == EOF) {
        clearerr(stdin);
        uint8_t b;
        if (s_usb_keys && usb_serial_jtag_read_bytes(&b, 1, 0) == 1) {
            c = b;
        }
    }
    return c;
}

/* gpu_start() and con_start() ran in app_main. */
void gpu_bringup_run(void) {
    keys_init();
    /* The boot log filled the whole screen; from now on it scrolls between the status rows
     * and the key row. Drawn at once: if the link isn't up yet, the shadow keeps it. */
    con_set_window(ROW_LOG, LOG_ROWS);
    io_bringup_init(ROW_IO);
    draw_screen();
    update_screen();
    gpu_present();
    if (gpu_wait_up(8000)) {
        print_stats();
    }
    printf("keys: s stats, f 4/10 MHz, r reset GPU,\n      t stress test, g demo\n");
    io_bringup_help();

    stress_t stress = {0};
    int64_t next_update = 0;
    for (;;) {
        int c = read_key();
        if (c != EOF && stress.on) {
            stress_end(&stress);
            continue;
        }
        switch (c) {
        case 's':
            print_stats();
            break;
        case 'f': {
            gpu_stats_t st;
            gpu_get_stats(&st);
            uint32_t hz = st.spi_hz >= HALTZ_GPU_SPI_HZ_MAX ? HALTZ_GPU_SPI_HZ_START
                                                            : HALTZ_GPU_SPI_HZ_MAX;
            gpu_set_spi_hz(hz);
            break;
        }
        case 'r':
            printf("gpu: resetting the GPU on request\n");
            gpu_force_reset();
            break;
        case 't':
            stress_begin(&stress);
            break;
        case 'g':
            demo_run();
            break;
        case EOF:
        case '\r':
        case '\n':
            break;
        default:
            if (!io_bringup_key(c)) {
                printf("keys: s stats, f 4/10 MHz, r reset GPU,\n      t stress test, g demo\n");
                io_bringup_help();
            }
            break;
        }

        bool pointer_moved = io_bringup_poll(!stress.on);
        if (stress.on) {
            stress_step(&stress);
            vTaskDelay(1);
            continue;
        }
        if (now_ms() >= next_update) {
            next_update = now_ms() + UPDATE_MS;
            update_screen();
            gpu_present();
        } else if (pointer_moved) {
            gpu_present();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
