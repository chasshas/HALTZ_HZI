/*
 * HALTZ GPU link: constants and bit layouts for the GPU_* messages in msg_ids.def.
 * The protocol is described in docs/gpu-protocol-v1.md; section numbers below refer to it.
 * Sizes, bits and colour numbers are part of the protocol. Link timings are S3 policy.
 */
#ifndef HALTZ_GPU_H
#define HALTZ_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. Transport (SPI link, S3 master -> Pico slave) ---------------------------------- */
#define HALTZ_GPU_SPI_MODE      3        /* CPOL=1, CPHA=1, MSB first, 8-bit */
#define HALTZ_GPU_SPI_HZ_START  4000000  /* bring-up speed */
#define HALTZ_GPU_SPI_HZ_MAX    10000000 /* Pico slave limit is clk_peri / 12 = 12.5 MHz */
#define HALTZ_GPU_FILLER        0x00u    /* sent between frames, both directions */
#define HALTZ_GPU_MAX_XFER      4096     /* bytes per transaction (CS low .. CS high), max */
#define HALTZ_GPU_POLL_XFER     64       /* filler transaction to collect replies */
#define HALTZ_GPU_RX_RING       32768    /* GPU receive ring, bytes */
#define HALTZ_GPU_BUSY_FREE     (2 * HALTZ_GPU_MAX_XFER) /* BUSY high below this much free */
#define HALTZ_GPU_FRAME_PULSE_US 2       /* FRAME high pulse, minimum */

/* ---- 2. Handshake and health (S3 side) --------------------------------------------------- */
#define HALTZ_GPU_RESET_MS         1    /* RESET held low at least this long */
#define HALTZ_GPU_HELLO_EVERY_MS   50   /* S3 repeats HELLO until the GPU answers */
#define HALTZ_GPU_HELLO_TIMEOUT_MS 2000 /* no HELLO back: reset again */
#define HALTZ_GPU_HELLO_TRIES      3    /* then: GPU missing */
#define HALTZ_GPU_STATUS_EVERY_MS  500  /* GPU_STATUS_REQ at least this often */
#define HALTZ_GPU_DEAD_MS          1000 /* no GPU_STATUS this long: reset */
#define HALTZ_GPU_BUSY_STUCK_MS    200  /* BUSY high this long: reset */
#define HALTZ_GPU_FRAME_TIMEOUT_MS 500  /* no FRAME pulse this long after PRESENT: reset */

/* ---- 3. Screen and layers ---------------------------------------------------------------- */
#define HALTZ_GPU_SCREEN_W 320
#define HALTZ_GPU_SCREEN_H 240

/* GPU_LAYERS mask */
#define HALTZ_GPU_LAYER_BITMAP   0x01u
#define HALTZ_GPU_LAYER_TILES    0x02u
#define HALTZ_GPU_LAYER_SPRITES  0x04u
#define HALTZ_GPU_LAYER_TEXT     0x08u
#define HALTZ_GPU_LAYERS_ALL     0x0Fu /* other bits must be 0 */
#define HALTZ_GPU_LAYERS_DEFAULT (HALTZ_GPU_LAYER_TEXT | HALTZ_GPU_LAYER_SPRITES)

/* ---- 3. Colours -------------------------------------------------------------------------- */
#define HALTZ_GPU_PALETTE_SIZE 256
#define HALTZ_GPU_TRANSPARENT  0 /* index 0: transparent in every layer; palette[0] = backdrop */

/* RGB565 from 8-bit r, g, b. */
#define HALTZ_GPU_RGB565(r, g, b)                                                              \
    ((uint16_t)((((unsigned)(r) & 0xF8u) << 8) | (((unsigned)(g) & 0xFCu) << 3) |             \
                ((unsigned)(b) >> 3)))

/* Default palette: 0 backdrop (black), 1-15 system colours, 16-231 6x6x6 cube, 232-255 greys. */
enum {
    HALTZ_COLOR_BACKDROP = 0,
    HALTZ_COLOR_WHITE = 1,
    HALTZ_COLOR_RED = 2,
    HALTZ_COLOR_GREEN = 3,
    HALTZ_COLOR_BLUE = 4,
    HALTZ_COLOR_YELLOW = 5,
    HALTZ_COLOR_CYAN = 6,
    HALTZ_COLOR_MAGENTA = 7,
    HALTZ_COLOR_DARK_GREY = 8,
    HALTZ_COLOR_LIGHT_GREY = 9,
    HALTZ_COLOR_ORANGE = 10,
    HALTZ_COLOR_BROWN = 11,
    HALTZ_COLOR_PINK = 12,
    HALTZ_COLOR_LIGHT_GREEN = 13,
    HALTZ_COLOR_LIGHT_BLUE = 14,
    HALTZ_COLOR_DARK_BLUE = 15,
    HALTZ_COLOR_BLACK = 16, /* opaque black (first entry of the colour cube) */
    HALTZ_COLOR_CUBE_FIRST = 16,
    HALTZ_COLOR_GREY_FIRST = 232,
};

/* ---- 3. Text layer ----------------------------------------------------------------------- */
#define HALTZ_GPU_TEXT_COLS   40
#define HALTZ_GPU_TEXT_ROWS   30
#define HALTZ_GPU_GLYPH_SIZE  8   /* 8 x 8 pixels */
#define HALTZ_GPU_GLYPH_BYTES 8   /* one byte per row, bit 7 = left pixel */
#define HALTZ_GPU_FONT_GLYPHS 256

/* Default font (assets/font8x8.txt): 0x20-0x7E ASCII, 0x00-0x1F and 0x7F blank, plus: */
enum {
    HALTZ_GLYPH_BOX_H = 0x80,          /* horizontal line */
    HALTZ_GLYPH_BOX_V = 0x81,          /* vertical line */
    HALTZ_GLYPH_BOX_DOWN_RIGHT = 0x82, /* top-left corner */
    HALTZ_GLYPH_BOX_DOWN_LEFT = 0x83,  /* top-right corner */
    HALTZ_GLYPH_BOX_UP_RIGHT = 0x84,   /* bottom-left corner */
    HALTZ_GLYPH_BOX_UP_LEFT = 0x85,    /* bottom-right corner */
    HALTZ_GLYPH_BOX_TEE_RIGHT = 0x86,  /* vertical line with a branch to the right */
    HALTZ_GLYPH_BOX_TEE_LEFT = 0x87,
    HALTZ_GLYPH_BOX_TEE_DOWN = 0x88,
    HALTZ_GLYPH_BOX_TEE_UP = 0x89,
    HALTZ_GLYPH_BOX_CROSS = 0x8A,
    HALTZ_GLYPH_BLOCK_FULL = 0x8B,
    HALTZ_GLYPH_BLOCK_UPPER = 0x8C,
    HALTZ_GLYPH_BLOCK_LOWER = 0x8D,
    HALTZ_GLYPH_BLOCK_LEFT = 0x8E,
    HALTZ_GLYPH_BLOCK_RIGHT = 0x8F,
    HALTZ_GLYPH_SHADE_LIGHT = 0x90, /* 25 % */
    HALTZ_GLYPH_SHADE_MEDIUM = 0x91, /* 50 % */
    HALTZ_GLYPH_SHADE_DARK = 0x92,   /* 75 % */
};

/* GPU_TEXT_CURSOR style */
#define HALTZ_GPU_CURSOR_OFF       0x00u
#define HALTZ_GPU_CURSOR_UNDERLINE 0x01u
#define HALTZ_GPU_CURSOR_BLOCK     0x02u
#define HALTZ_GPU_CURSOR_BLINK     0x80u /* or-ed with UNDERLINE / BLOCK; GPU blinks at 2 Hz */

/* ---- 3. Patterns (shared by tiles and sprites) ------------------------------------------- */
#define HALTZ_GPU_PATTERNS      256
#define HALTZ_GPU_PATTERN_SIZE  16  /* 16 x 16 pixels, 4 bits each */
#define HALTZ_GPU_PATTERN_BYTES 128 /* 16 rows x 8 bytes, high nibble = left pixel */

/* ---- 3. Tile layer ----------------------------------------------------------------------- */
#define HALTZ_GPU_MAP_W  32 /* tiles */
#define HALTZ_GPU_MAP_H  32
#define HALTZ_GPU_MAP_PX (HALTZ_GPU_MAP_W * HALTZ_GPU_PATTERN_SIZE) /* 512, scroll wraps here */

/* Tile map entry (u16) */
#define HALTZ_GPU_TILE_PATTERN_MASK 0x00FFu
#define HALTZ_GPU_TILE_BANK_SHIFT   8
#define HALTZ_GPU_TILE_BANK_MASK    0x0F00u
#define HALTZ_GPU_TILE_HFLIP        0x1000u
#define HALTZ_GPU_TILE_VFLIP        0x2000u
#define HALTZ_GPU_TILE_RESERVED     0xC000u /* must be 0 */
#define HALTZ_GPU_TILE(pattern, bank, flips)                                                   \
    ((uint16_t)(((unsigned)(pattern) & 0xFFu) | (((unsigned)(bank) & 0x0Fu) << 8) |            \
                ((unsigned)(flips) & (HALTZ_GPU_TILE_HFLIP | HALTZ_GPU_TILE_VFLIP))))

/* ---- 3. Sprites -------------------------------------------------------------------------- */
#define HALTZ_GPU_SPRITES       64
#define HALTZ_GPU_SPRITE_CURSOR 63 /* kernel convention: the pointer, with ABOVE_TEXT */

/* Sprite attribute (u8) */
#define HALTZ_GPU_SPR_BANK_MASK  0x0Fu
#define HALTZ_GPU_SPR_HFLIP      0x10u
#define HALTZ_GPU_SPR_VFLIP      0x20u
#define HALTZ_GPU_SPR_ABOVE_TEXT 0x40u
#define HALTZ_GPU_SPR_VISIBLE    0x80u

/* ---- 4. GPU_STATUS flags ----------------------------------------------------------------- */
#define HALTZ_GPU_STATUS_BUSY      0x01u /* BUSY line is high */
#define HALTZ_GPU_STATUS_RENDERING 0x02u /* a frame is being drawn */

/* ---- Default tables (src/gpu_tables.c, generated by tools/gen_gpu_tables.py) ------------- */
/* What GPU_INIT loads. The S3 shadow and fallback renderer use the same tables. */
extern const uint16_t haltz_gpu_default_palette[HALTZ_GPU_PALETTE_SIZE]; /* RGB565 */
extern const uint8_t haltz_gpu_default_font[HALTZ_GPU_FONT_GLYPHS][HALTZ_GPU_GLYPH_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_GPU_H */
