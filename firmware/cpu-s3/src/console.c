/*
 * HALTZ CPU: text console on the TFT. See console.h.
 */
#include "console.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "con_term.h"
#include "gpu.h"

#define RING_SIZE  4096 /* power of two */
#define CHUNK      256  /* bytes taken out of the ring at a time */
#define TASK_STACK 4096
#define TASK_PRIO  6 /* below the gpu task (8), above the bring-up console (5) */
#define TASK_CORE  0

#define REQ_WINDOW  0x01u
#define REQ_REDRAW  0x02u
#define REQ_HOLD    0x04u
#define REQ_RELEASE 0x08u
#define REQ_CLEAR   0x10u

/* Ring and requests: shared with every printing task, s_mux. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_ring[RING_SIZE];
static uint32_t s_head, s_tail; /* free-running byte counts */
static uint32_t s_dropped;
static uint32_t s_req;
static uint8_t s_req_top, s_req_height;

static TaskHandle_t s_task;
static int s_orig_fd = -1; /* the normal console (UART0 + native USB) */

/* Console task only */
static con_term_t s_term;
static bool s_held;

/* ---- ring -------------------------------------------------------------------------------- */

static void wake(void) {
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void con_write(const void *data, size_t n) {
    const uint8_t *p = data;
    portENTER_CRITICAL(&s_mux);
    size_t space = RING_SIZE - (size_t)(s_head - s_tail);
    size_t k = n < space ? n : space;
    for (size_t i = 0; i < k; i++) {
        s_ring[(s_head + i) & (RING_SIZE - 1)] = p[i];
    }
    s_head += (uint32_t)k;
    s_dropped += (uint32_t)(n - k);
    portEXIT_CRITICAL(&s_mux);
    if (k > 0) {
        wake();
    }
}

static size_t ring_take(uint8_t *out, size_t cap) {
    portENTER_CRITICAL(&s_mux);
    size_t n = (size_t)(s_head - s_tail);
    n = n < cap ? n : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(s_tail + i) & (RING_SIZE - 1)];
    }
    s_tail += (uint32_t)n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

uint32_t con_dropped(void) {
    return s_dropped;
}

/* ---- stdout tee: /dev/hzcon --------------------------------------------------------------- */

static int hz_open(const char *path, int flags, int mode) {
    (void)path;
    (void)flags;
    (void)mode;
    return 0;
}

static ssize_t hz_write(int fd, const void *data, size_t size) {
    (void)fd;
    if (s_orig_fd >= 0) {
        (void)write(s_orig_fd, data, size);
    }
    con_write(data, size);
    return (ssize_t)size;
}

static int hz_close(int fd) {
    (void)fd;
    return 0;
}

static int hz_fstat(int fd, struct stat *st) {
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static const esp_vfs_fs_ops_t s_hz_ops = {
    .write = hz_write,
    .open = hz_open,
    .close = hz_close,
    .fstat = hz_fstat,
};

esp_err_t con_early_init(void) {
    s_orig_fd = open("/dev/console", O_WRONLY);
    if (s_orig_fd < 0) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_vfs_register_fs("/dev/hzcon", &s_hz_ops, ESP_VFS_FLAG_STATIC, NULL);
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen("/dev/hzcon", "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    setvbuf(f, NULL, _IOLBF, 256);
    fflush(stdout);
    /* ESP-IDF tasks share the global reent's FILE pointers (esp_reent_init): tasks created
     * from now on use f; this task needs its own pointer changed too. */
    _REENT_STDOUT(_GLOBAL_REENT) = f;
    stdout = f;
    return ESP_OK;
}

/* ---- console task ------------------------------------------------------------------------ */

static void emit_gpu(const haltz_gpu_cmd_t *cmd, void *ctx) {
    (void)ctx;
    if (!s_held) {
        gpu_cmd(cmd);
    }
}

static void request(uint32_t req) {
    portENTER_CRITICAL(&s_mux);
    s_req |= req;
    portEXIT_CRITICAL(&s_mux);
    wake();
}

static void con_task(void *arg) {
    (void)arg;
    static uint8_t buf[CHUNK];
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        uint32_t req = s_req;
        s_req = 0;
        uint8_t top = s_req_top, height = s_req_height;
        portEXIT_CRITICAL(&s_mux);

        bool changed = false;
        if (req & REQ_HOLD) {
            s_held = true;
        }
        if (req & REQ_RELEASE) {
            s_held = false;
            req |= REQ_REDRAW;
        }
        if (req & REQ_WINDOW) {
            con_term_set_window(&s_term, top, height);
            req |= REQ_REDRAW; /* the lines may have moved */
        }
        if (req & REQ_CLEAR) {
            con_term_clear(&s_term);
            changed = true;
        }
        if (req & REQ_REDRAW) {
            con_term_redraw(&s_term);
            changed = true;
        }
        for (size_t n; (n = ring_take(buf, sizeof(buf))) > 0;) {
            con_term_feed(&s_term, buf, n);
            changed = true;
        }
        if (changed) {
            con_term_flush(&s_term);
            if (!s_held) {
                gpu_present(); /* waits while the previous frame is on its way */
            }
        }
        /* Anything written meanwhile has notified us: then this returns at once. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

esp_err_t con_start(uint8_t top, uint8_t height) {
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    con_term_init(&s_term, top, height, emit_gpu, NULL);
    s_req = REQ_CLEAR;
    if (xTaskCreatePinnedToCore(con_task, "con", TASK_STACK, NULL, TASK_PRIO, &s_task,
                                TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void con_set_window(uint8_t top, uint8_t height) {
    portENTER_CRITICAL(&s_mux);
    s_req_top = top;
    s_req_height = height;
    portEXIT_CRITICAL(&s_mux);
    request(REQ_WINDOW);
}

void con_redraw(void) {
    request(REQ_REDRAW);
}

void con_hold(bool hold) {
    request(hold ? REQ_HOLD : REQ_RELEASE);
}
