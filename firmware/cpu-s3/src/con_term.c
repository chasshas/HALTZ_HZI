#include "con_term.h"

#include <string.h>

#define TAB 8

/* ANSI colours 0-7 (black red green yellow blue magenta cyan white) as HALTZ system colours. */
static const uint8_t k_ansi_fg[8] = {HALTZ_COLOR_BLACK,  HALTZ_COLOR_RED,
                                     HALTZ_COLOR_GREEN,  HALTZ_COLOR_YELLOW,
                                     HALTZ_COLOR_BLUE,   HALTZ_COLOR_MAGENTA,
                                     HALTZ_COLOR_CYAN,   HALTZ_COLOR_LIGHT_GREY};
static const uint8_t k_ansi_bright[8] = {HALTZ_COLOR_DARK_GREY,   HALTZ_COLOR_PINK,
                                         HALTZ_COLOR_LIGHT_GREEN, HALTZ_COLOR_YELLOW,
                                         HALTZ_COLOR_LIGHT_BLUE,  HALTZ_COLOR_MAGENTA,
                                         HALTZ_COLOR_CYAN,        HALTZ_COLOR_WHITE};

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void send(con_term_t *t) {
    t->emit(&t->cmd, t->ctx);
}

static int bottom(const con_term_t *t) {
    return t->top + t->height; /* exclusive */
}

static void fill_cells(con_term_t *t, int row0, int row1, uint8_t fg, uint8_t bg) {
    for (int r = row0; r < row1; r++) {
        memset(t->ch[r], ' ', CON_COLS);
        memset(t->cfg[r], fg, CON_COLS);
        memset(t->cbg[r], bg, CON_COLS);
    }
}

/* ---- setup ------------------------------------------------------------------------------- */

void con_term_init(con_term_t *t, uint8_t top, uint8_t height, con_term_emit_fn emit,
                   void *ctx) {
    memset(t, 0, sizeof(*t));
    t->emit = emit;
    t->ctx = ctx;
    t->fg = CON_FG_DEFAULT;
    t->bg = CON_BG_DEFAULT;
    fill_cells(t, 0, CON_ROWS, CON_FG_DEFAULT, CON_BG_DEFAULT);
    t->sent_col = t->sent_row = 0xFF;
    con_term_set_window(t, top, height);
    t->col = 0;
    t->row = t->top;
}

void con_term_set_window(con_term_t *t, uint8_t top, uint8_t height) {
    int old_row = t->row;
    t->top = (uint8_t)clampi(top, 0, CON_ROWS - 1);
    t->height = (uint8_t)clampi(height, 1, CON_ROWS - t->top);
    int bot = bottom(t);
    if (old_row >= bot) {
        /* The cursor is below the new window: move the newest lines up into it, the cursor's
         * line onto the window's last row. */
        int shift = old_row - (bot - 1);
        for (int r = t->top; r < bot; r++) {
            memcpy(t->ch[r], t->ch[r + shift], CON_COLS);
            memcpy(t->cfg[r], t->cfg[r + shift], CON_COLS);
            memcpy(t->cbg[r], t->cbg[r + shift], CON_COLS);
        }
        t->row = (uint8_t)(bot - 1);
        t->run_len = 0; /* its characters moved with the cells */
    } else if (old_row < t->top) {
        t->row = t->top;
    }
}

void con_term_set_color(con_term_t *t, uint8_t fg, uint8_t bg) {
    t->fg = fg;
    t->bg = bg;
}

/* ---- output ------------------------------------------------------------------------------ */

static void flush_run(con_term_t *t) {
    if (t->run_len == 0) {
        return;
    }
    t->cmd.type = HALTZ_MSG_GPU_TEXT_PUT;
    haltz_gpu_text_put_t *m = &t->cmd.u.text_put;
    m->col = t->run_col;
    m->row = t->run_row;
    m->fg = t->run_fg;
    m->bg = t->run_bg;
    m->count = t->run_len;
    memcpy(m->chars, t->run, t->run_len);
    send(t);
    t->run_len = 0;
}

static void scroll_up(con_term_t *t) {
    flush_run(t); /* its row is about to move */
    int top = t->top, bot = bottom(t);
    for (int r = top; r < bot - 1; r++) {
        memcpy(t->ch[r], t->ch[r + 1], CON_COLS);
        memcpy(t->cfg[r], t->cfg[r + 1], CON_COLS);
        memcpy(t->cbg[r], t->cbg[r + 1], CON_COLS);
    }
    fill_cells(t, bot - 1, bot, t->fg, t->bg);
    t->cmd.type = HALTZ_MSG_GPU_TEXT_SCROLL;
    t->cmd.u.text_scroll = (haltz_gpu_text_scroll_t){t->top, t->height, 1, ' ', t->fg, t->bg};
    send(t);
}

static void newline(con_term_t *t) {
    t->col = 0;
    if (t->row + 1 >= bottom(t)) {
        scroll_up(t);
    } else {
        t->row++;
    }
}

static void put_char(con_term_t *t, uint8_t ch) {
    if (t->col >= CON_COLS) {
        newline(t); /* deferred wrap */
    }
    int r = t->row, c = t->col;
    t->ch[r][c] = ch;
    t->cfg[r][c] = t->fg;
    t->cbg[r][c] = t->bg;
    bool extends = t->run_len > 0 && t->run_row == r && t->run_col + t->run_len == c &&
                   t->run_fg == t->fg && t->run_bg == t->bg;
    if (!extends) {
        flush_run(t);
        t->run_col = (uint8_t)c;
        t->run_row = (uint8_t)r;
        t->run_fg = t->fg;
        t->run_bg = t->bg;
    }
    t->run[t->run_len++] = ch;
    t->col++;
}

/* ---- ANSI SGR ---------------------------------------------------------------------------- */

static void sgr(con_term_t *t) {
    if (t->nparams == 0) {
        t->fg = CON_FG_DEFAULT;
        t->bg = CON_BG_DEFAULT;
        return;
    }
    for (int i = 0; i < t->nparams; i++) {
        unsigned p = t->params[i];
        if (p == 0) {
            t->fg = CON_FG_DEFAULT;
            t->bg = CON_BG_DEFAULT;
        } else if (p >= 30 && p <= 37) {
            t->fg = k_ansi_fg[p - 30];
        } else if (p == 39) {
            t->fg = CON_FG_DEFAULT;
        } else if (p >= 40 && p <= 47) {
            t->bg = p == 40 ? CON_BG_DEFAULT : k_ansi_fg[p - 40]; /* black = the backdrop */
        } else if (p == 49) {
            t->bg = CON_BG_DEFAULT;
        } else if (p >= 90 && p <= 97) {
            t->fg = k_ansi_bright[p - 90];
        } else if (p >= 100 && p <= 107) {
            t->bg = k_ansi_bright[p - 100];
        }
        /* bold, underline, ...: ignored */
    }
}

/* One byte inside an escape sequence. Returns false if the byte ended the sequence without
 * being part of it (a control byte): the caller then handles it as a normal byte. */
static bool escape_byte(con_term_t *t, uint8_t b) {
    if (t->esc == 1) {
        if (b == '[') {
            t->esc = 2;
            t->nparams = 0;
            return true;
        }
        t->esc = 0; /* ESC + anything else: both dropped (control bytes still act) */
        return b >= 0x20;
    }
    /* inside CSI */
    if (b >= '0' && b <= '9') {
        if (t->nparams == 0) {
            t->params[t->nparams++] = 0;
        }
        uint16_t *p = &t->params[t->nparams - 1];
        *p = (uint16_t)(*p < 1000 ? *p * 10 + (b - '0') : *p);
        return true;
    }
    if (b == ';') {
        if (t->nparams == 0) {
            t->params[t->nparams++] = 0;
        }
        if (t->nparams < sizeof(t->params) / sizeof(t->params[0])) {
            t->params[t->nparams++] = 0;
        }
        return true;
    }
    if (b >= 0x20 && b <= 0x3F) {
        return true; /* other parameter / intermediate bytes ('?', ' ', ...): ignored */
    }
    t->esc = 0;
    if (b >= 0x40 && b <= 0x7E) {
        if (b == 'm') {
            sgr(t);
        }
        return true; /* final byte */
    }
    return false; /* a control byte (or 0x7F+) aborts the sequence */
}

void con_term_feed(con_term_t *t, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (t->esc != 0 && escape_byte(t, b)) {
            continue;
        }
        switch (b) {
        case '\n':
            newline(t);
            break;
        case '\r':
            t->col = 0;
            break;
        case '\b':
            if (t->col > 0) {
                t->col--;
            }
            break;
        case '\t': {
            int next = (t->col / TAB + 1) * TAB;
            t->col = (uint8_t)(next > CON_COLS ? CON_COLS : next);
            break;
        }
        case 0x1B:
            t->esc = 1;
            break;
        default:
            if (b >= 0x20 && b != 0x7F) {
                put_char(t, b);
            }
            break;
        }
    }
}

/* ---- flush, clear, redraw ---------------------------------------------------------------- */

static void send_cursor(con_term_t *t) {
    uint8_t c = (uint8_t)(t->col >= CON_COLS ? CON_COLS - 1 : t->col);
    if (c == t->sent_col && t->row == t->sent_row) {
        return;
    }
    t->cmd.type = HALTZ_MSG_GPU_TEXT_CURSOR;
    t->cmd.u.text_cursor = (haltz_gpu_text_cursor_t){
        c, t->row, HALTZ_GPU_CURSOR_UNDERLINE | HALTZ_GPU_CURSOR_BLINK};
    send(t);
    t->sent_col = c;
    t->sent_row = t->row;
}

void con_term_flush(con_term_t *t) {
    flush_run(t);
    send_cursor(t);
}

void con_term_clear(con_term_t *t) {
    t->run_len = 0;
    fill_cells(t, t->top, bottom(t), t->fg, t->bg);
    t->cmd.type = HALTZ_MSG_GPU_TEXT_FILL;
    t->cmd.u.text_fill = (haltz_gpu_text_fill_t){0, t->top, CON_COLS, t->height, ' ', t->fg,
                                                 t->bg};
    send(t);
    t->col = 0;
    t->row = t->top;
}

static bool cell_default(const con_term_t *t, int r, int c) {
    return t->ch[r][c] == ' ' && t->cfg[r][c] == CON_FG_DEFAULT && t->cbg[r][c] == CON_BG_DEFAULT;
}

void con_term_redraw(con_term_t *t) {
    t->run_len = 0; /* its characters are in the cells */
    t->cmd.type = HALTZ_MSG_GPU_TEXT_FILL;
    t->cmd.u.text_fill = (haltz_gpu_text_fill_t){0, t->top, CON_COLS, t->height, ' ',
                                                 CON_FG_DEFAULT, CON_BG_DEFAULT};
    send(t);
    for (int r = t->top; r < bottom(t); r++) {
        int col = 0;
        while (col < CON_COLS) {
            if (cell_default(t, r, col)) {
                col++;
                continue;
            }
            /* Same colours, without default cells at the end (as gpu_replay.c does). */
            int c0 = col, last = col;
            uint8_t fg = t->cfg[r][c0], bg = t->cbg[r][c0];
            while (col < CON_COLS && t->cfg[r][col] == fg && t->cbg[r][col] == bg) {
                if (!cell_default(t, r, col)) {
                    last = col;
                }
                col++;
            }
            t->cmd.type = HALTZ_MSG_GPU_TEXT_PUT;
            haltz_gpu_text_put_t *m = &t->cmd.u.text_put;
            m->col = (uint8_t)c0;
            m->row = (uint8_t)r;
            m->fg = fg;
            m->bg = bg;
            m->count = (uint8_t)(last - c0 + 1);
            memcpy(m->chars, &t->ch[r][c0], m->count);
            send(t);
        }
    }
    t->sent_col = t->sent_row = 0xFF; /* send the cursor again */
    send_cursor(t);
}
