#include "haltz/gpu_msgs.h"

#include <string.h>

#include "haltz/msgs.h"
#include "wire.h"

/* The element limits must match the sizes in msg_ids.def. */
_Static_assert(HALTZ_MSG_GPU_PALETTE_MAX_LEN == 1 + 2 * HALTZ_GPU_PALETTE_MAX, "palette");
_Static_assert(HALTZ_MSG_GPU_PALETTE_MIN_LEN == 1 + 2, "palette");
_Static_assert(HALTZ_MSG_GPU_TEXT_PUT_MAX_LEN == 4 + HALTZ_GPU_TEXT_PUT_MAX, "text_put");
_Static_assert(HALTZ_MSG_GPU_TEXT_PUT_MIN_LEN == 4 + 1, "text_put");
_Static_assert(HALTZ_MSG_GPU_FONT_LOAD_MAX_LEN ==
                   1 + HALTZ_GPU_FONT_LOAD_MAX * HALTZ_GPU_GLYPH_BYTES, "font_load");
_Static_assert(HALTZ_MSG_GPU_FONT_LOAD_MIN_LEN == 1 + HALTZ_GPU_GLYPH_BYTES, "font_load");
_Static_assert(HALTZ_MSG_GPU_PATTERN_LOAD_LEN == 1 + HALTZ_GPU_PATTERN_BYTES, "pattern_load");
_Static_assert(HALTZ_MSG_GPU_TILE_PUT_MAX_LEN == 2 + 2 * HALTZ_GPU_TILE_PUT_MAX, "tile_put");
_Static_assert(HALTZ_MSG_GPU_TILE_PUT_MIN_LEN == 2 + 2, "tile_put");
_Static_assert(HALTZ_MSG_GPU_BMP_PIXELS_MAX_LEN == 4 + HALTZ_GPU_BMP_PIXELS_MAX, "bmp_pixels");
_Static_assert(HALTZ_MSG_GPU_BMP_PIXELS_MIN_LEN == 4 + 1, "bmp_pixels");

/* ---- helpers ----------------------------------------------------------------------------- */

static haltz_err_t pack_args(const void *msg, const uint8_t *buf) {
    return (msg == NULL || buf == NULL) ? HALTZ_ERR_ARG : HALTZ_OK;
}

static haltz_err_t pack_space(size_t cap, size_t need) {
    return (cap < need) ? HALTZ_ERR_NO_SPACE : HALTZ_OK;
}

static haltz_err_t pack_done(size_t *out_len, size_t pos) {
    if (out_len != NULL) {
        *out_len = pos;
    }
    return HALTZ_OK;
}

/* Arguments, then the LEN range from msg_ids.def. */
static haltz_err_t unpack_args(const void *msg, const uint8_t *buf, uint8_t type, size_t len) {
    if (msg == NULL || (buf == NULL && len > 0)) {
        return HALTZ_ERR_ARG;
    }
    return haltz_msg_check(type, len);
}

/* first .. first + count - 1 must be a valid table index (table has 256 entries). */
static int range_ok(uint8_t first, uint8_t count, uint8_t max_count) {
    return count >= 1 && count <= max_count && (unsigned)first + count <= 256u;
}

/* ---- value checks (shared by pack and unpack) -------------------------------------------- */

static haltz_err_t check_layers(const haltz_gpu_layers_t *m) {
    return (m->mask & ~HALTZ_GPU_LAYERS_ALL) ? HALTZ_ERR_BAD_VALUE : HALTZ_OK;
}

static haltz_err_t check_palette(const haltz_gpu_palette_t *m) {
    return range_ok(m->first, m->count, HALTZ_GPU_PALETTE_MAX) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_text_put(const haltz_gpu_text_put_t *m) {
    return (m->col < HALTZ_GPU_TEXT_COLS && m->row < HALTZ_GPU_TEXT_ROWS && m->count >= 1 &&
            m->count <= HALTZ_GPU_TEXT_PUT_MAX)
               ? HALTZ_OK
               : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_text_cursor(const haltz_gpu_text_cursor_t *m) {
    uint8_t shape = (uint8_t)(m->style & ~HALTZ_GPU_CURSOR_BLINK);
    return (shape <= HALTZ_GPU_CURSOR_BLOCK) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_font_load(const haltz_gpu_font_load_t *m) {
    return range_ok(m->first, m->count, HALTZ_GPU_FONT_LOAD_MAX) ? HALTZ_OK
                                                                  : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_tile_entry(uint16_t entry) {
    return (entry & HALTZ_GPU_TILE_RESERVED) ? HALTZ_ERR_BAD_VALUE : HALTZ_OK;
}

static haltz_err_t check_tile_put(const haltz_gpu_tile_put_t *m) {
    if (m->x >= HALTZ_GPU_MAP_W || m->y >= HALTZ_GPU_MAP_H || m->count < 1 ||
        m->count > HALTZ_GPU_TILE_PUT_MAX) {
        return HALTZ_ERR_BAD_VALUE;
    }
    for (int i = 0; i < m->count; i++) {
        if (check_tile_entry(m->entries[i]) != HALTZ_OK) {
            return HALTZ_ERR_BAD_VALUE;
        }
    }
    return HALTZ_OK;
}

static haltz_err_t check_tile_fill(const haltz_gpu_tile_fill_t *m) {
    return check_tile_entry(m->entry);
}

static haltz_err_t check_sprite_set(const haltz_gpu_sprite_set_t *m) {
    return (m->id < HALTZ_GPU_SPRITES) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_bmp_pixels(const haltz_gpu_bmp_pixels_t *m) {
    return (m->count >= 1 && m->count <= HALTZ_GPU_BMP_PIXELS_MAX) ? HALTZ_OK
                                                                    : HALTZ_ERR_BAD_VALUE;
}

/* ---- control ----------------------------------------------------------------------------- */

haltz_err_t haltz_gpu_present_pack(const haltz_gpu_present_t *msg, uint8_t *buf, size_t cap,
                                   size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_PRESENT_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u16le(buf, &pos, msg->id);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_present_unpack(haltz_gpu_present_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_PRESENT, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->id = get_u16le(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_backlight_pack(const haltz_gpu_backlight_t *msg, uint8_t *buf, size_t cap,
                                     size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_BACKLIGHT_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->level);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_backlight_unpack(haltz_gpu_backlight_t *msg, const uint8_t *buf,
                                       size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_BACKLIGHT, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->level = buf[0];
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_layers_pack(const haltz_gpu_layers_t *msg, uint8_t *buf, size_t cap,
                                  size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_layers(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_LAYERS_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->mask);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_layers_unpack(haltz_gpu_layers_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_LAYERS, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->mask = buf[0];
    return check_layers(msg);
}

haltz_err_t haltz_gpu_palette_pack(const haltz_gpu_palette_t *msg, uint8_t *buf, size_t cap,
                                   size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_palette(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, 1u + 2u * msg->count);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->first);
    for (int i = 0; i < msg->count; i++) {
        put_u16le(buf, &pos, msg->colors[i]);
    }
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_palette_unpack(haltz_gpu_palette_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_PALETTE, len);
    if (err != HALTZ_OK) {
        return err;
    }
    if ((len - 1) % 2 != 0) {
        return HALTZ_ERR_BAD_LEN;
    }
    size_t pos = 0;
    msg->first = get_u8(buf, &pos);
    msg->count = (uint8_t)((len - 1) / 2);
    for (int i = 0; i < msg->count; i++) {
        msg->colors[i] = get_u16le(buf, &pos);
    }
    return check_palette(msg);
}

/* ---- text layer -------------------------------------------------------------------------- */

haltz_err_t haltz_gpu_text_put_pack(const haltz_gpu_text_put_t *msg, uint8_t *buf, size_t cap,
                                    size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_text_put(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, 4u + msg->count);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->col);
    put_u8(buf, &pos, msg->row);
    put_u8(buf, &pos, msg->fg);
    put_u8(buf, &pos, msg->bg);
    memcpy(&buf[pos], msg->chars, msg->count);
    pos += msg->count;
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_text_put_unpack(haltz_gpu_text_put_t *msg, const uint8_t *buf,
                                      size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TEXT_PUT, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->col = get_u8(buf, &pos);
    msg->row = get_u8(buf, &pos);
    msg->fg = get_u8(buf, &pos);
    msg->bg = get_u8(buf, &pos);
    msg->count = (uint8_t)(len - pos);
    memcpy(msg->chars, &buf[pos], msg->count);
    return check_text_put(msg);
}

haltz_err_t haltz_gpu_text_fill_pack(const haltz_gpu_text_fill_t *msg, uint8_t *buf, size_t cap,
                                     size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_TEXT_FILL_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->col);
    put_u8(buf, &pos, msg->row);
    put_u8(buf, &pos, msg->w);
    put_u8(buf, &pos, msg->h);
    put_u8(buf, &pos, msg->ch);
    put_u8(buf, &pos, msg->fg);
    put_u8(buf, &pos, msg->bg);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_text_fill_unpack(haltz_gpu_text_fill_t *msg, const uint8_t *buf,
                                       size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TEXT_FILL, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->col = get_u8(buf, &pos);
    msg->row = get_u8(buf, &pos);
    msg->w = get_u8(buf, &pos);
    msg->h = get_u8(buf, &pos);
    msg->ch = get_u8(buf, &pos);
    msg->fg = get_u8(buf, &pos);
    msg->bg = get_u8(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_text_scroll_pack(const haltz_gpu_text_scroll_t *msg, uint8_t *buf,
                                       size_t cap, size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_TEXT_SCROLL_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->top);
    put_u8(buf, &pos, msg->height);
    put_u8(buf, &pos, (uint8_t)msg->rows); /* two's complement */
    put_u8(buf, &pos, msg->ch);
    put_u8(buf, &pos, msg->fg);
    put_u8(buf, &pos, msg->bg);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_text_scroll_unpack(haltz_gpu_text_scroll_t *msg, const uint8_t *buf,
                                         size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TEXT_SCROLL, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->top = get_u8(buf, &pos);
    msg->height = get_u8(buf, &pos);
    uint8_t r = get_u8(buf, &pos);
    msg->rows = (r & 0x80u) ? (int8_t)(-(int)(0x100u - r)) : (int8_t)r;
    msg->ch = get_u8(buf, &pos);
    msg->fg = get_u8(buf, &pos);
    msg->bg = get_u8(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_text_cursor_pack(const haltz_gpu_text_cursor_t *msg, uint8_t *buf,
                                       size_t cap, size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_text_cursor(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_TEXT_CURSOR_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->col);
    put_u8(buf, &pos, msg->row);
    put_u8(buf, &pos, msg->style);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_text_cursor_unpack(haltz_gpu_text_cursor_t *msg, const uint8_t *buf,
                                         size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TEXT_CURSOR, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->col = get_u8(buf, &pos);
    msg->row = get_u8(buf, &pos);
    msg->style = get_u8(buf, &pos);
    return check_text_cursor(msg);
}

haltz_err_t haltz_gpu_font_load_pack(const haltz_gpu_font_load_t *msg, uint8_t *buf, size_t cap,
                                     size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_font_load(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, 1u + (size_t)msg->count * HALTZ_GPU_GLYPH_BYTES);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->first);
    memcpy(&buf[pos], msg->glyphs, (size_t)msg->count * HALTZ_GPU_GLYPH_BYTES);
    pos += (size_t)msg->count * HALTZ_GPU_GLYPH_BYTES;
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_font_load_unpack(haltz_gpu_font_load_t *msg, const uint8_t *buf,
                                       size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_FONT_LOAD, len);
    if (err != HALTZ_OK) {
        return err;
    }
    if ((len - 1) % HALTZ_GPU_GLYPH_BYTES != 0) {
        return HALTZ_ERR_BAD_LEN;
    }
    msg->first = buf[0];
    msg->count = (uint8_t)((len - 1) / HALTZ_GPU_GLYPH_BYTES);
    memcpy(msg->glyphs, &buf[1], len - 1);
    return check_font_load(msg);
}

/* ---- patterns, tile layer, sprites ------------------------------------------------------- */

haltz_err_t haltz_gpu_pattern_load_pack(const haltz_gpu_pattern_load_t *msg, uint8_t *buf,
                                        size_t cap, size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_PATTERN_LOAD_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->index);
    memcpy(&buf[pos], msg->data, HALTZ_GPU_PATTERN_BYTES);
    pos += HALTZ_GPU_PATTERN_BYTES;
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_pattern_load_unpack(haltz_gpu_pattern_load_t *msg, const uint8_t *buf,
                                          size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_PATTERN_LOAD, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->index = buf[0];
    memcpy(msg->data, &buf[1], HALTZ_GPU_PATTERN_BYTES);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_tile_put_pack(const haltz_gpu_tile_put_t *msg, uint8_t *buf, size_t cap,
                                    size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_tile_put(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, 2u + 2u * msg->count);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->x);
    put_u8(buf, &pos, msg->y);
    for (int i = 0; i < msg->count; i++) {
        put_u16le(buf, &pos, msg->entries[i]);
    }
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_tile_put_unpack(haltz_gpu_tile_put_t *msg, const uint8_t *buf,
                                      size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TILE_PUT, len);
    if (err != HALTZ_OK) {
        return err;
    }
    if ((len - 2) % 2 != 0) {
        return HALTZ_ERR_BAD_LEN;
    }
    size_t pos = 0;
    msg->x = get_u8(buf, &pos);
    msg->y = get_u8(buf, &pos);
    msg->count = (uint8_t)((len - 2) / 2);
    for (int i = 0; i < msg->count; i++) {
        msg->entries[i] = get_u16le(buf, &pos);
    }
    return check_tile_put(msg);
}

haltz_err_t haltz_gpu_tile_fill_pack(const haltz_gpu_tile_fill_t *msg, uint8_t *buf, size_t cap,
                                     size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_tile_fill(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_TILE_FILL_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->x);
    put_u8(buf, &pos, msg->y);
    put_u8(buf, &pos, msg->w);
    put_u8(buf, &pos, msg->h);
    put_u16le(buf, &pos, msg->entry);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_tile_fill_unpack(haltz_gpu_tile_fill_t *msg, const uint8_t *buf,
                                       size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TILE_FILL, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->x = get_u8(buf, &pos);
    msg->y = get_u8(buf, &pos);
    msg->w = get_u8(buf, &pos);
    msg->h = get_u8(buf, &pos);
    msg->entry = get_u16le(buf, &pos);
    return check_tile_fill(msg);
}

haltz_err_t haltz_gpu_tile_scroll_pack(const haltz_gpu_tile_scroll_t *msg, uint8_t *buf,
                                       size_t cap, size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_TILE_SCROLL_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u16le(buf, &pos, msg->sx);
    put_u16le(buf, &pos, msg->sy);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_tile_scroll_unpack(haltz_gpu_tile_scroll_t *msg, const uint8_t *buf,
                                         size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_TILE_SCROLL, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->sx = get_u16le(buf, &pos);
    msg->sy = get_u16le(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_sprite_set_pack(const haltz_gpu_sprite_set_t *msg, uint8_t *buf,
                                      size_t cap, size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_sprite_set(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_SPRITE_SET_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->id);
    put_i16le(buf, &pos, msg->x);
    put_i16le(buf, &pos, msg->y);
    put_u8(buf, &pos, msg->pattern);
    put_u8(buf, &pos, msg->attr);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_sprite_set_unpack(haltz_gpu_sprite_set_t *msg, const uint8_t *buf,
                                        size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_SPRITE_SET, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->id = get_u8(buf, &pos);
    msg->x = get_i16le(buf, &pos);
    msg->y = get_i16le(buf, &pos);
    msg->pattern = get_u8(buf, &pos);
    msg->attr = get_u8(buf, &pos);
    return check_sprite_set(msg);
}

/* ---- bitmap layer ------------------------------------------------------------------------ */

haltz_err_t haltz_gpu_bmp_pixels_pack(const haltz_gpu_bmp_pixels_t *msg, uint8_t *buf,
                                      size_t cap, size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = check_bmp_pixels(msg);
    }
    if (err == HALTZ_OK) {
        err = pack_space(cap, 4u + msg->count);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u16le(buf, &pos, msg->x);
    put_u16le(buf, &pos, msg->y);
    memcpy(&buf[pos], msg->pixels, msg->count);
    pos += msg->count;
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_bmp_pixels_unpack(haltz_gpu_bmp_pixels_t *msg, const uint8_t *buf,
                                        size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_BMP_PIXELS, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->x = get_u16le(buf, &pos);
    msg->y = get_u16le(buf, &pos);
    msg->count = (uint8_t)(len - pos);
    memcpy(msg->pixels, &buf[pos], msg->count);
    return check_bmp_pixels(msg);
}

haltz_err_t haltz_gpu_bmp_fill_pack(const haltz_gpu_bmp_fill_t *msg, uint8_t *buf, size_t cap,
                                    size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_BMP_FILL_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u16le(buf, &pos, msg->x);
    put_u16le(buf, &pos, msg->y);
    put_u16le(buf, &pos, msg->w);
    put_u16le(buf, &pos, msg->h);
    put_u8(buf, &pos, msg->color);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_bmp_fill_unpack(haltz_gpu_bmp_fill_t *msg, const uint8_t *buf,
                                      size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_BMP_FILL, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->x = get_u16le(buf, &pos);
    msg->y = get_u16le(buf, &pos);
    msg->w = get_u16le(buf, &pos);
    msg->h = get_u16le(buf, &pos);
    msg->color = get_u8(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_bmp_line_pack(const haltz_gpu_bmp_line_t *msg, uint8_t *buf, size_t cap,
                                    size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_BMP_LINE_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_i16le(buf, &pos, msg->x0);
    put_i16le(buf, &pos, msg->y0);
    put_i16le(buf, &pos, msg->x1);
    put_i16le(buf, &pos, msg->y1);
    put_u8(buf, &pos, msg->color);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_bmp_line_unpack(haltz_gpu_bmp_line_t *msg, const uint8_t *buf,
                                      size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_BMP_LINE, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->x0 = get_i16le(buf, &pos);
    msg->y0 = get_i16le(buf, &pos);
    msg->x1 = get_i16le(buf, &pos);
    msg->y1 = get_i16le(buf, &pos);
    msg->color = get_u8(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_gpu_bmp_copy_pack(const haltz_gpu_bmp_copy_t *msg, uint8_t *buf, size_t cap,
                                    size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_BMP_COPY_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u16le(buf, &pos, msg->sx);
    put_u16le(buf, &pos, msg->sy);
    put_u16le(buf, &pos, msg->w);
    put_u16le(buf, &pos, msg->h);
    put_i16le(buf, &pos, msg->dx);
    put_i16le(buf, &pos, msg->dy);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_bmp_copy_unpack(haltz_gpu_bmp_copy_t *msg, const uint8_t *buf,
                                      size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_BMP_COPY, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->sx = get_u16le(buf, &pos);
    msg->sy = get_u16le(buf, &pos);
    msg->w = get_u16le(buf, &pos);
    msg->h = get_u16le(buf, &pos);
    msg->dx = get_i16le(buf, &pos);
    msg->dy = get_i16le(buf, &pos);
    return HALTZ_OK;
}

/* ---- GPU -> S3 --------------------------------------------------------------------------- */

haltz_err_t haltz_gpu_status_pack(const haltz_gpu_status_t *msg, uint8_t *buf, size_t cap,
                                  size_t *out_len) {
    haltz_err_t err = pack_args(msg, buf);
    if (err == HALTZ_OK) {
        err = pack_space(cap, HALTZ_MSG_GPU_STATUS_LEN);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u16le(buf, &pos, msg->present_id);
    put_u16le(buf, &pos, msg->frames);
    put_u16le(buf, &pos, msg->render_us);
    put_u8(buf, &pos, msg->rx_free_kb);
    put_u8(buf, &pos, msg->flags);
    put_u16le(buf, &pos, msg->crc_errors);
    put_u16le(buf, &pos, msg->cmd_errors);
    put_u16le(buf, &pos, msg->rx_overruns);
    put_u32le(buf, &pos, msg->uptime_ms);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_gpu_status_unpack(haltz_gpu_status_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_args(msg, buf, HALTZ_MSG_GPU_STATUS, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->present_id = get_u16le(buf, &pos);
    msg->frames = get_u16le(buf, &pos);
    msg->render_us = get_u16le(buf, &pos);
    msg->rx_free_kb = get_u8(buf, &pos);
    msg->flags = get_u8(buf, &pos);
    msg->crc_errors = get_u16le(buf, &pos);
    msg->cmd_errors = get_u16le(buf, &pos);
    msg->rx_overruns = get_u16le(buf, &pos);
    msg->uptime_ms = get_u32le(buf, &pos);
    return HALTZ_OK;
}

/* ---- any S3 -> GPU command --------------------------------------------------------------- */

haltz_err_t haltz_gpu_cmd_unpack(haltz_gpu_cmd_t *cmd, uint8_t type, const uint8_t *buf,
                                 size_t len) {
    if (cmd == NULL) {
        return HALTZ_ERR_ARG;
    }
    cmd->type = type;
    switch (type) {
    case HALTZ_MSG_GPU_INIT:
    case HALTZ_MSG_GPU_STATUS_REQ:
        return unpack_args(cmd, buf, type, len);
    case HALTZ_MSG_GPU_PRESENT:
        return haltz_gpu_present_unpack(&cmd->u.present, buf, len);
    case HALTZ_MSG_GPU_BACKLIGHT:
        return haltz_gpu_backlight_unpack(&cmd->u.backlight, buf, len);
    case HALTZ_MSG_GPU_LAYERS:
        return haltz_gpu_layers_unpack(&cmd->u.layers, buf, len);
    case HALTZ_MSG_GPU_PALETTE:
        return haltz_gpu_palette_unpack(&cmd->u.palette, buf, len);
    case HALTZ_MSG_GPU_TEXT_PUT:
        return haltz_gpu_text_put_unpack(&cmd->u.text_put, buf, len);
    case HALTZ_MSG_GPU_TEXT_FILL:
        return haltz_gpu_text_fill_unpack(&cmd->u.text_fill, buf, len);
    case HALTZ_MSG_GPU_TEXT_SCROLL:
        return haltz_gpu_text_scroll_unpack(&cmd->u.text_scroll, buf, len);
    case HALTZ_MSG_GPU_TEXT_CURSOR:
        return haltz_gpu_text_cursor_unpack(&cmd->u.text_cursor, buf, len);
    case HALTZ_MSG_GPU_FONT_LOAD:
        return haltz_gpu_font_load_unpack(&cmd->u.font_load, buf, len);
    case HALTZ_MSG_GPU_PATTERN_LOAD:
        return haltz_gpu_pattern_load_unpack(&cmd->u.pattern_load, buf, len);
    case HALTZ_MSG_GPU_TILE_PUT:
        return haltz_gpu_tile_put_unpack(&cmd->u.tile_put, buf, len);
    case HALTZ_MSG_GPU_TILE_FILL:
        return haltz_gpu_tile_fill_unpack(&cmd->u.tile_fill, buf, len);
    case HALTZ_MSG_GPU_TILE_SCROLL:
        return haltz_gpu_tile_scroll_unpack(&cmd->u.tile_scroll, buf, len);
    case HALTZ_MSG_GPU_SPRITE_SET:
        return haltz_gpu_sprite_set_unpack(&cmd->u.sprite_set, buf, len);
    case HALTZ_MSG_GPU_BMP_PIXELS:
        return haltz_gpu_bmp_pixels_unpack(&cmd->u.bmp_pixels, buf, len);
    case HALTZ_MSG_GPU_BMP_FILL:
        return haltz_gpu_bmp_fill_unpack(&cmd->u.bmp_fill, buf, len);
    case HALTZ_MSG_GPU_BMP_LINE:
        return haltz_gpu_bmp_line_unpack(&cmd->u.bmp_line, buf, len);
    case HALTZ_MSG_GPU_BMP_COPY:
        return haltz_gpu_bmp_copy_unpack(&cmd->u.bmp_copy, buf, len);
    default:
        return HALTZ_ERR_UNKNOWN_TYPE;
    }
}
