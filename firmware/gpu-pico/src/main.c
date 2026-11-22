/*
 * HALTZ GPU (Pico 2 W #1, RP2350): firmware for docs/gpu-protocol-v1.md.
 *
 * Core 0: the S3 link and the command loop.
 *   Reads frames from the receive ring, applies drawing commands to the picture state,
 *   answers HELLO and GPU_STATUS_REQ, NAKs bad commands, drives BUSY.
 *   At GPU_PRESENT it hands the changed rectangles to core 1 as a render job and applies
 *   nothing more until the job is done, so the panel always shows the state at a PRESENT.
 *   Also: text cursor blink (small render jobs), the demo, and the console (the board's own
 *   USB port, and UART0 for a debug probe).
 * Core 1: owns the panel. Draws each job's rectangles in strips of up to 320 x 16 pixels,
 *   rendering one strip while DMA sends the previous one, then pulses FRAME for a PRESENT.
 *
 * The picture state, renderer and scenes are portable (gpu_state.c, gpu_render.c,
 * gpu_scenes.c) and have PC tests in firmware/gpu-pico/test.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#if LIB_PICO_STDIO_USB
#include "pico/stdio_usb.h"
#endif

#include "board_pins.h"
#include "gpu_render.h"
#include "gpu_scenes.h"
#include "gpu_state.h"
#include "haltz/board_ids.h"
#include "haltz/gpu_msgs.h"
#include "haltz/msgs.h"
#include "haltz/packet.h"
#include "haltz/proto.h"
#include "link.h"
#include "tft.h"

#if defined(__has_include)
#if __has_include("haltz_git_hash.h")
#include "haltz_git_hash.h"
#endif
#endif
#ifndef HALTZ_GIT_HASH
#define HALTZ_GIT_HASH "00000000"
#endif

#define STRIP_PIXELS    (HALTZ_GPU_SCREEN_W * 16) /* one strip buffer, RGB565 */
#define BLINK_HALF_MS   250                       /* cursor blink: 2 Hz */
#define DEMO_FRAME_MS   33
#define EVENTS_PER_LOOP 64 /* frames handled before servicing TX/BUSY again */

/* ---- shared between the cores (core 0 writes only while no job runs) --------------------- */

typedef enum { JOB_TFT_INIT, JOB_PRESENT, JOB_REFRESH, JOB_BLINK } job_kind_t;

typedef struct {
    gpu_rect_t rects[GPU_MAX_RECTS + 1]; /* + the cursor cell */
    int count;
    bool blink_on;
    bool pulse_frame;
} render_job_t;

static gpu_state_t g_state;
static render_job_t g_job;
static volatile uint32_t g_job_render_us; /* core 1: time spent composing in the last job */

/* ---- core 1: renderer -------------------------------------------------------------------- */

static uint16_t g_strip[2][STRIP_PIXELS];
static uint32_t g_core1_stack[1024]; /* 4 KB */

static void run_job(const render_job_t *job) {
    int k = 0;
    uint32_t render_us = 0;
    for (int i = 0; i < job->count; i++) {
        const gpu_rect_t *r = &job->rects[i];
        int rows = STRIP_PIXELS / r->w;
        tft_begin_rect(r->x, r->y, r->w, r->h);
        for (int y = 0; y < r->h; y += rows) {
            int h = (r->h - y < rows) ? r->h - y : rows;
            /* Draw into one buffer while DMA still sends the other. */
            uint32_t t0 = time_us_32();
            gpu_render_rect(&g_state, r->x, r->y + y, r->w, h, job->blink_on, g_strip[k]);
            render_us += time_us_32() - t0;
            tft_send(g_strip[k], (size_t)r->w * (size_t)h);
            k ^= 1;
        }
        tft_end_rect();
    }
    g_job_render_us = render_us;
}

static void core1_main(void) {
    tft_init();
    __dmb();
    multicore_fifo_push_blocking(0); /* "panel ready" = end of JOB_TFT_INIT */
    for (;;) {
        (void)multicore_fifo_pop_blocking();
        __dmb();
        uint32_t t0 = time_us_32();
        run_job(&g_job);
        if (g_job.pulse_frame) {
            link_frame_pulse();
        }
        uint32_t us = time_us_32() - t0;
        __dmb();
        multicore_fifo_push_blocking(us);
    }
}

/* ---- core 0: state ----------------------------------------------------------------------- */

static haltz_decoder_t g_dec;
static bool g_dec_pending; /* the decoder may hold more events: poll before feeding */
static haltz_gpu_cmd_t g_cmd;

static bool g_job_busy = true; /* JOB_TFT_INIT runs from the start */
static job_kind_t g_job_kind = JOB_TFT_INIT;
static uint16_t g_job_present_id;
static bool g_drawn_blink_on = true;

static uint16_t g_present_id;
static uint16_t g_frames;
static uint16_t g_render_us;
static uint16_t g_crc_errors;
static uint16_t g_cmd_errors;
static uint32_t g_flushes;
static bool g_cpu_seen;
static bool g_tft_ready;

static bool g_demo;
static uint32_t g_demo_frame;
static absolute_time_t g_demo_next;
static uint32_t g_demo_drawn;    /* demo frames on the panel */
static uint32_t g_demo_start_ms;

/* Console statistics of the drawing (core 1 time per job, including the transfer). */
static int g_draw_rects;     /* last PRESENT / refresh job */
static uint32_t g_draw_px;
static uint32_t g_draw_us;
static uint32_t g_draw_max_us;
static uint32_t g_draw_render_us; /* of g_draw_us: composing pixels; the rest is the SPI */
static uint32_t g_blink_us;       /* last cursor-cell job (the smallest possible update) */

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static bool blink_phase_on(void) {
    return (now_ms() / BLINK_HALF_MS) % 2 == 0;
}

/* ---- render jobs ------------------------------------------------------------------------- */

static void submit_job(job_kind_t kind, bool pulse_frame) {
    g_job.count = gpu_state_take_dirty(&g_state, g_job.rects, GPU_MAX_RECTS);
    g_job.blink_on = blink_phase_on();
    gpu_rect_t cr;
    if (gpu_state_cursor_blinks(&g_state) && gpu_state_cursor_rect(&g_state, &cr)) {
        /* Draw the cursor cell too (keeps the drawn blink phase in step), unless one of the
         * rectangles already covers it. */
        bool covered = false;
        for (int i = 0; i < g_job.count && !covered; i++) {
            const gpu_rect_t *r = &g_job.rects[i];
            covered = cr.x >= r->x && cr.y >= r->y && cr.x + cr.w <= r->x + r->w &&
                      cr.y + cr.h <= r->y + r->h;
        }
        if (!covered) {
            g_job.rects[g_job.count++] = cr;
        }
    }
    g_job.pulse_frame = pulse_frame;
    g_drawn_blink_on = g_job.blink_on;
    g_job_kind = kind;
    g_job_busy = true;
    __dmb();
    multicore_fifo_push_blocking(0);
}

/* Redraw only the cursor cell when its blink phase changes (no FRAME pulse). */
static void submit_blink_job(void) {
    gpu_rect_t cr;
    if (!gpu_state_cursor_blinks(&g_state) || !gpu_state_cursor_rect(&g_state, &cr) ||
        blink_phase_on() == g_drawn_blink_on) {
        return;
    }
    g_job.rects[0] = cr;
    g_job.count = 1;
    g_job.blink_on = blink_phase_on();
    g_job.pulse_frame = false;
    g_drawn_blink_on = g_job.blink_on;
    g_job_kind = JOB_BLINK;
    g_job_busy = true;
    __dmb();
    multicore_fifo_push_blocking(0);
}

static void job_done(uint32_t us) {
    g_job_busy = false;
    if (g_job_kind == JOB_BLINK) {
        g_blink_us = us;
    } else if (g_job_kind != JOB_TFT_INIT) {
        uint32_t px = 0;
        for (int i = 0; i < g_job.count; i++) {
            px += (uint32_t)(g_job.rects[i].w * g_job.rects[i].h);
        }
        g_draw_rects = g_job.count;
        g_draw_px = px;
        g_draw_us = us;
        g_draw_render_us = g_job_render_us;
        g_draw_max_us = us > g_draw_max_us ? us : g_draw_max_us;
        if (g_demo) {
            g_demo_drawn++;
        }
    }
    if (g_job_kind == JOB_TFT_INIT) {
        g_tft_ready = true;
        printf("tft: SPI1 at %u Hz, panel ready\n", tft_spi_hz());
        submit_job(JOB_REFRESH, false); /* boot screen (or whatever the S3 drew already) */
        return;
    }
    if (g_job_kind == JOB_PRESENT) {
        g_render_us = us > 0xFFFFu ? 0xFFFFu : (uint16_t)us; /* GPU_STATUS: frames only */
        g_present_id = g_job_present_id;
        g_frames++;
    }
}

/* ---- replies ----------------------------------------------------------------------------- */

static void send_hello(void) {
    haltz_hello_t h = {.board_id = HALTZ_BOARD_GPU, .proto_version = HALTZ_PROTO_VERSION};
    haltz_git_hash_parse(HALTZ_GIT_HASH, h.git_hash);
    uint8_t p[HALTZ_MSG_HELLO_LEN];
    haltz_hello_pack(&h, p, sizeof(p));
    link_send(HALTZ_MSG_HELLO, p, sizeof(p));
}

static void send_nak(uint8_t type, haltz_err_t err) {
    g_cmd_errors++;
    if (g_cmd_errors <= 8 || g_cmd_errors % 256 == 0) {
        printf("link: NAK %s (0x%02X): error %d (%u so far)\n", haltz_msg_name(type), type,
               (int)err, (unsigned)g_cmd_errors);
    }
    haltz_nak_t nak = {.type = type, .error = (uint8_t)err};
    uint8_t p[HALTZ_MSG_NAK_LEN];
    haltz_nak_pack(&nak, p, sizeof(p));
    link_send(HALTZ_MSG_NAK, p, sizeof(p)); /* best effort: dropped if the queue is full */
}

static void send_status(void) {
    uint32_t rx_free = link_rx_free();
    haltz_gpu_status_t st = {
        .present_id = g_present_id,
        .frames = g_frames,
        .render_us = g_render_us,
        .rx_free_kb = (uint8_t)(rx_free / 1024),
        .flags = (uint8_t)((link_busy() ? HALTZ_GPU_STATUS_BUSY : 0) |
                           (g_job_busy ? HALTZ_GPU_STATUS_RENDERING : 0)),
        .crc_errors = g_crc_errors,
        .cmd_errors = g_cmd_errors,
        .rx_overruns = (uint16_t)link_rx_overruns(),
        .uptime_ms = now_ms(),
    };
    uint8_t p[HALTZ_MSG_GPU_STATUS_LEN];
    haltz_gpu_status_pack(&st, p, sizeof(p), NULL);
    link_send(HALTZ_MSG_GPU_STATUS, p, sizeof(p));
}

/* ---- command loop ------------------------------------------------------------------------ */

static void stop_demo(const char *why) {
    if (!g_demo) {
        return;
    }
    g_demo = false;
    printf("demo: stopped (%s)\n", why);
    if (!g_cpu_seen) {
        gpu_scene_boot(&g_state, HALTZ_PROTO_VERSION, HALTZ_GIT_HASH);
    }
}

static void handle_hello(const haltz_frame_t *f) {
    haltz_hello_t h;
    if (haltz_hello_unpack(&h, f->payload, f->len) != HALTZ_OK) {
        send_nak(f->type, HALTZ_ERR_BAD_LEN);
        return;
    }
    char git[9];
    haltz_git_hash_format(h.git_hash, git);
    printf("link: HELLO from board %u, protocol v%u, git %s%s\n", h.board_id,
           h.proto_version, git,
           h.proto_version != HALTZ_PROTO_VERSION ? "  (PROTOCOL VERSION DIFFERS!)" : "");
    g_cpu_seen = true;
    send_hello();
}

static void handle_frame(const haltz_frame_t *f) {
    stop_demo("the CPU is talking");
    if (f->type == HALTZ_MSG_HELLO) {
        handle_hello(f);
        return;
    }
    if (f->type == HALTZ_MSG_ACK || f->type == HALTZ_MSG_NAK) {
        return; /* not used towards the GPU */
    }
    haltz_err_t err = haltz_gpu_cmd_unpack(&g_cmd, f->type, f->payload, f->len);
    if (err != HALTZ_OK) {
        send_nak(f->type, err);
        return;
    }
    switch (g_cmd.type) {
    case HALTZ_MSG_GPU_PRESENT:
        g_job_present_id = g_cmd.u.present.id;
        submit_job(JOB_PRESENT, true);
        break;
    case HALTZ_MSG_GPU_STATUS_REQ:
        send_status();
        break;
    case HALTZ_MSG_GPU_BACKLIGHT:
        tft_backlight(g_cmd.u.backlight.level);
        break;
    default:
        gpu_state_apply(&g_state, &g_cmd);
        break;
    }
}

/* Next decoder event from the receive ring, or HALTZ_DEC_NEED_MORE. */
static haltz_dec_result_t next_event(void) {
    if (g_dec_pending) {
        haltz_dec_result_t r = haltz_decoder_poll(&g_dec);
        if (r != HALTZ_DEC_NEED_MORE) {
            return r;
        }
        g_dec_pending = false;
    }
    const uint8_t *p;
    bool overflow;
    size_t n = link_rx_peek(&p, &overflow);
    if (overflow) {
        printf("link: receive ring overrun, data lost (the S3 ignored BUSY)\n");
        haltz_decoder_init(&g_dec);
    }
    for (size_t i = 0; i < n; i++) {
        haltz_dec_result_t r = haltz_decoder_feed(&g_dec, p[i]);
        if (r != HALTZ_DEC_NEED_MORE) {
            link_rx_consume(i + 1);
            g_dec_pending = true;
            return r;
        }
    }
    link_rx_consume(n);
    return HALTZ_DEC_NEED_MORE;
}

static void process_link(void) {
    for (int i = 0; i < EVENTS_PER_LOOP && !g_job_busy; i++) {
        haltz_dec_result_t r = next_event();
        if (r == HALTZ_DEC_NEED_MORE) {
            /* The S3 never splits a frame over two transactions: a half frame left over
             * after CS went high is a glitch, so give up on it. */
            if (!link_rx_idle_between_transactions()) {
                break;
            }
            uint32_t before = g_dec.flushes;
            r = haltz_decoder_flush(&g_dec);
            g_flushes += g_dec.flushes - before;
            if (r == HALTZ_DEC_NEED_MORE) {
                break;
            }
            g_dec_pending = true;
        }
        if (r == HALTZ_DEC_FRAME) {
            handle_frame(haltz_decoder_frame(&g_dec));
        } else if (r == HALTZ_DEC_CRC_ERROR) {
            g_crc_errors++;
        }
    }
}

/* ---- console (USB and UART0) ------------------------------------------------------------- */

#define KEYS_HELP "keys: d = demo, s = stats, v = version\n"

static void print_banner(void) {
    printf("\n==========================================\n");
    printf(" HALTZ GPU\n");
    printf(" protocol v%u, git %s\n", (unsigned)HALTZ_PROTO_VERSION, HALTZ_GIT_HASH);
    printf("==========================================\n");
    if (g_tft_ready) {
        printf("tft: SPI1 at %u Hz, panel ready\n", tft_spi_hz());
    } else {
        printf("tft: starting\n");
    }
    printf("cpu: %s\n", g_cpu_seen ? "connected (HELLO received)" : "waiting for HELLO");
    printf(KEYS_HELP);
}

static void print_stats(void) {
    printf("draw: last %d rect(s), %lu px in %lu us (composing %lu us); slowest %lu us\n",
           g_draw_rects, (unsigned long)g_draw_px, (unsigned long)g_draw_us,
           (unsigned long)g_draw_render_us, (unsigned long)g_draw_max_us);
    if (g_blink_us) {
        printf("draw: cursor cell alone %lu us\n", (unsigned long)g_blink_us);
    }
    if (g_demo) {
        uint32_t ms = now_ms() - g_demo_start_ms;
        uint32_t fps10 = ms ? g_demo_drawn * 10000u / ms : 0;
        printf("demo: %lu frames drawn in %lu ms = %lu.%lu fps\n", (unsigned long)g_demo_drawn,
               (unsigned long)ms, (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10));
    }
    printf("link: frames %u (last PRESENT id %u), crc errors %u, NAKs %u, overruns %lu, "
           "flushes %lu, rx free %lu B, BUSY %d, uptime %lu ms\n",
           (unsigned)g_frames, (unsigned)g_present_id, (unsigned)g_crc_errors,
           (unsigned)g_cmd_errors, (unsigned long)link_rx_overruns(), (unsigned long)g_flushes,
           (unsigned long)link_rx_free(), link_busy(), (unsigned long)now_ms());
}

static void console(void) {
#if LIB_PICO_STDIO_USB
    /* USB comes up after boot, so greet each time a terminal opens the port. */
    static bool usb_was_connected;
    bool usb_connected = stdio_usb_connected();
    if (usb_connected && !usb_was_connected) {
        print_banner();
    }
    usb_was_connected = usb_connected;
#endif
    int c = getchar_timeout_us(0);
    if (c < 0) {
        return;
    }
    if (g_demo && c != 's') {
        stop_demo("key");
        return;
    }
    if (c == 'd') {
        if (g_cpu_seen) {
            printf("demo: not while the CPU drives the screen (reset the GPU first)\n");
            return;
        }
        printf("demo: running; s = stats, any other key stops it\n");
        gpu_scene_demo_init(&g_state);
        g_demo = true;
        g_demo_frame = 0;
        g_demo_drawn = 0;
        g_demo_start_ms = now_ms();
        g_demo_next = get_absolute_time();
    } else if (c == 's') {
        print_stats();
    } else if (c == 'v') {
        print_banner();
    } else if (c != '\r' && c != '\n') {
        printf(KEYS_HELP);
    }
}

/* ---- main -------------------------------------------------------------------------------- */

int main(void) {
    link_init(); /* first: BUSY high as early as possible */
    stdio_init_all();
    print_banner(); /* reaches UART0 now; USB terminals get it when they connect */

    gpu_scene_boot(&g_state, HALTZ_PROTO_VERSION, HALTZ_GIT_HASH);
    multicore_launch_core1_with_stack(core1_main, g_core1_stack, sizeof(g_core1_stack));

    haltz_decoder_init(&g_dec);
    send_hello(); /* waits in the send queue until the S3 clocks it out */
    link_set_ready(true);

    for (;;) {
        link_poll();
        if (g_job_busy && multicore_fifo_rvalid()) {
            uint32_t us = multicore_fifo_pop_blocking();
            __dmb();
            job_done(us);
        }
        if (!g_job_busy) {
            process_link();
        }
        if (!g_job_busy && g_demo && time_reached(g_demo_next)) {
            g_demo_next = make_timeout_time_ms(DEMO_FRAME_MS);
            gpu_scene_demo_step(&g_state, ++g_demo_frame);
            submit_job(JOB_REFRESH, false);
        }
        if (!g_job_busy && !g_demo && gpu_state_is_dirty(&g_state) && !g_cpu_seen) {
            submit_job(JOB_REFRESH, false); /* boot screen after the demo */
        }
        if (!g_job_busy) {
            submit_blink_job();
        }
        console();
    }
}
