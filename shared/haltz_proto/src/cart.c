#include "haltz/cart.h"

#include <string.h>

#include "haltz/crc16.h"

static int title_ok(const char *t, size_t max) {
    size_t n = 0;
    while (n < max && t[n] != '\0') {
        if ((unsigned char)t[n] < 0x20u || (unsigned char)t[n] > 0x7Eu) {
            return 0;
        }
        n++;
    }
    return n <= max;
}

static int fields_ok(const haltz_cart_header_t *h) {
    if (h->type > HALTZ_CART_DATA || h->flags != 0) {
        return 0;
    }
    if (h->type == HALTZ_CART_KEY ? h->key_id == 0 : h->key_id != 0) {
        return 0;
    }
    return 1;
}

haltz_err_t haltz_cart_header_pack(const haltz_cart_header_t *h, const uint8_t *body,
                                   uint8_t out[HALTZ_CART_HEADER_LEN]) {
    if (h == NULL || out == NULL || (body == NULL && h->length > 0)) {
        return HALTZ_ERR_ARG;
    }
    if (!fields_ok(h) || memchr(h->title, '\0', sizeof h->title) == NULL ||
        !title_ok(h->title, HALTZ_CART_TITLE_LEN)) {
        return HALTZ_ERR_BAD_VALUE;
    }
    memset(out, 0, HALTZ_CART_HEADER_LEN);
    out[0] = 'H';
    out[1] = 'Z';
    out[2] = HALTZ_CART_FORMAT;
    out[3] = h->type;
    memcpy(&out[4], h->title, strlen(h->title));
    out[20] = (uint8_t)h->length;
    out[21] = (uint8_t)(h->length >> 8);
    out[22] = h->flags;
    out[24] = (uint8_t)h->key_id;
    out[25] = (uint8_t)(h->key_id >> 8);
    out[26] = (uint8_t)(h->key_id >> 16);
    out[27] = (uint8_t)(h->key_id >> 24);
    uint16_t crc = haltz_cart_crc(out, body, h->length);
    out[30] = (uint8_t)crc;
    out[31] = (uint8_t)(crc >> 8);
    return HALTZ_OK;
}

haltz_err_t haltz_cart_header_unpack(const uint8_t in[HALTZ_CART_HEADER_LEN],
                                     haltz_cart_header_t *h) {
    if (in == NULL || h == NULL) {
        return HALTZ_ERR_ARG;
    }
    if (in[0] != 'H' || in[1] != 'Z') {
        return HALTZ_ERR_BAD_VALUE;
    }
    if (in[2] != HALTZ_CART_FORMAT) {
        return HALTZ_ERR_VERSION;
    }
    memset(h, 0, sizeof *h);
    h->type = in[3];
    /* Title: printable up to the first 0, then only 0s. */
    size_t n = 0;
    while (n < HALTZ_CART_TITLE_LEN && in[4 + n] != 0) {
        n++;
    }
    for (size_t i = n; i < HALTZ_CART_TITLE_LEN; i++) {
        if (in[4 + i] != 0) {
            return HALTZ_ERR_BAD_VALUE;
        }
    }
    memcpy(h->title, &in[4], n);
    h->title[n] = '\0';
    h->length = (uint16_t)(in[20] | (in[21] << 8));
    h->flags = in[22];
    h->key_id = (uint32_t)in[24] | ((uint32_t)in[25] << 8) | ((uint32_t)in[26] << 16) |
                ((uint32_t)in[27] << 24);
    h->crc = (uint16_t)(in[30] | (in[31] << 8));
    if (in[23] != 0 || in[28] != 0 || in[29] != 0 || !fields_ok(h) ||
        !title_ok(h->title, HALTZ_CART_TITLE_LEN)) {
        return HALTZ_ERR_BAD_VALUE;
    }
    return HALTZ_OK;
}

uint16_t haltz_cart_crc(const uint8_t hdr[HALTZ_CART_HEADER_LEN], const uint8_t *body,
                        size_t len) {
    uint16_t crc = haltz_crc16_update(HALTZ_CRC16_INIT, hdr, HALTZ_CART_CRC_OFFSET);
    return haltz_crc16_update(crc, body, len);
}

haltz_err_t haltz_cart_check(const uint8_t hdr[HALTZ_CART_HEADER_LEN], const uint8_t *body,
                             size_t len, haltz_cart_header_t *h) {
    haltz_cart_header_t tmp;
    if (h == NULL) {
        h = &tmp;
    }
    haltz_err_t err = haltz_cart_header_unpack(hdr, h);
    if (err != HALTZ_OK) {
        return err;
    }
    if (len < h->length || (body == NULL && h->length > 0)) {
        return HALTZ_ERR_BAD_LEN;
    }
    if (haltz_cart_crc(hdr, body, h->length) != h->crc) {
        return HALTZ_ERR_BAD_CRC;
    }
    return HALTZ_OK;
}

/* Byte offset in an area of 48-byte sectors starting at first_sector -> block number. */
static int area_block(unsigned offset, unsigned first_sector, unsigned size) {
    if (offset >= size) {
        return -1;
    }
    unsigned sector = first_sector + offset / 48u;
    unsigned block_in_sector = (offset % 48u) / HALTZ_CART_BLOCK_LEN;
    return (int)(sector * 4u + block_in_sector);
}

int haltz_cart_body_block(unsigned offset) {
    return area_block(offset, HALTZ_CART_BODY_SECTOR, HALTZ_CART_BODY_MAX);
}

int haltz_cart_save_block(unsigned offset) {
    return area_block(offset, HALTZ_CART_SAVE_SECTOR, HALTZ_CART_SAVE_LEN);
}
