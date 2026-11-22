/*
 * HALTZ CPU: text terminal engine for the console on the GPU's text layer.
 *
 * Turns a byte stream (what printf writes) into GPU text commands: characters are collected
 * into runs and sent as GPU_TEXT_PUT, scrolling is GPU_TEXT_SCROLL of the window, the cursor
 * is GPU_TEXT_CURSOR. The engine keeps its own copy of the window's cells, so the window can
 * be drawn again after something else cleared the screen (con_term_redraw).
 *
 * The window is a band of whole rows (top .. top + height - 1, all 40 columns); rows outside
 * it are never touched, so a status line can stay put while the log scrolls.
 *
 * Bytes:
 *   printable (0x20-0x7E) and 0x80-0xFF   drawn (0x80+ are the font's box/block glyphs)
 *   \n  next row, column 0       \r  column 0       \b  one column left
 *   \t  next multiple of 8       other control bytes and DEL: ignored
 *   ESC [ params letter          ANSI CSI sequence; SGR (letter m) sets colours:
 *                                0 reset, 30-37 / 90-97 fg, 40-47 / 100-107 bg, 39 / 49
 *                                default. Other sequences are skipped, never printed.
 * Writing column 40 wraps (deferred, like a terminal: "40 characters then \n" is one row).
 *
 * Portable C (no ESP-IDF): PC test in firmware/cpu-s3/test/test_con_term.c.
 */
#ifndef HALTZ_CPU_CON_TERM_H
#define HALTZ_CPU_CON_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "haltz/gpu.h"
#include "haltz/gpu_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CON_COLS HALTZ_GPU_TEXT_COLS
#define CON_ROWS HALTZ_GPU_TEXT_ROWS
#define CON_FG_DEFAULT HALTZ_COLOR_WHITE
#define CON_BG_DEFAULT 0 /* transparent: the backdrop shows */

typedef void (*con_term_emit_fn)(const haltz_gpu_cmd_t *cmd, void *ctx);

typedef struct {
    uint8_t top, height; /* window rows */
    uint8_t col, row;    /* cursor; row is a screen row; col == CON_COLS: wrap pending */
    uint8_t fg, bg;

    /* Own copy of the cells (whole screen, only the window rows are used). */
    uint8_t ch[CON_ROWS][CON_COLS];
    uint8_t cfg[CON_ROWS][CON_COLS];
    uint8_t cbg[CON_ROWS][CON_COLS];

    /* Characters written but not sent yet: one run on one row, one colour pair. */
    uint8_t run_col, run_row, run_len, run_fg, run_bg;
    uint8_t run[CON_COLS];

    /* ANSI escape parser */
    uint8_t esc;         /* 0 normal, 1 after ESC, 2 inside CSI */
    uint8_t nparams;
    uint16_t params[8];

    /* Cursor as last sent to the GPU (0xFF: not sent yet). */
    uint8_t sent_col, sent_row;

    con_term_emit_fn emit;
    void *ctx;
    haltz_gpu_cmd_t cmd; /* scratch for emit */
} con_term_t;

/* Reset everything (cells to the GPU_INIT defaults, cursor to the window's top left).
 * Sends nothing: call con_term_clear or con_term_redraw to put it on the screen. */
void con_term_init(con_term_t *t, uint8_t top, uint8_t height, con_term_emit_fn emit,
                   void *ctx);

/* Move the window (clamped to the screen, height >= 1). Sends nothing: call con_term_redraw
 * after it. The cells are kept; if the cursor is below the new window, the cells move up so
 * the newest lines (ending with the cursor's line) fill the window. */
void con_term_set_window(con_term_t *t, uint8_t top, uint8_t height);

void con_term_set_color(con_term_t *t, uint8_t fg, uint8_t bg);

/* Blank the window in the current colours, cursor to its top left. */
void con_term_clear(con_term_t *t);

void con_term_feed(con_term_t *t, const uint8_t *data, size_t n);

/* Send the pending run and the cursor position. */
void con_term_flush(con_term_t *t);

/* Send the whole window from the own cells (blank it, then the non-default runs), and the
 * cursor. For after a GPU_INIT or anything else drawn over the window. */
void con_term_redraw(con_term_t *t);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CPU_CON_TERM_H */
