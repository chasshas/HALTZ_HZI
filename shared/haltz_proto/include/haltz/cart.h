/*
 * HALTZ program images and cartridges (docs/vm-v1.md sections 7 and 8).
 *
 * One image format everywhere: a 32-byte header, then the body (the program or the data).
 * On a MIFARE Classic 1K card the header fills sector 0 blocks 1-2 and the body sectors 1-13;
 * in flash and on the PC it is a .hzp file, the header followed by the body.
 *
 * Header, all fields little-endian:
 *   0  magic     2 bytes  'H' 'Z'
 *   2  format    u8       HALTZ_CART_FORMAT
 *   3  type      u8       haltz_cart_type_t
 *   4  title     16 bytes printable ASCII (0x20-0x7E), padded with 0
 *   20 length    u16      body bytes
 *   22 flags     u8       0 (reserved)
 *   23 reserved  u8       0
 *   24 key_id    u32      KEY: the program's ID in flash (not 0); CODE, DATA: 0
 *   28 reserved  u16      0
 *   30 crc       u16      CRC-16 (crc16.h) of bytes 0-29, then of the body
 */
#ifndef HALTZ_CART_H
#define HALTZ_CART_H

#include <stddef.h>
#include <stdint.h>

#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALTZ_CART_FORMAT     1u
#define HALTZ_CART_HEADER_LEN 32u
#define HALTZ_CART_TITLE_LEN  16u
#define HALTZ_CART_CRC_OFFSET 30u

/* MIFARE Classic 1K: 16 sectors of 4 blocks of 16 bytes; block 3 of each sector is the
 * sector trailer (keys and access bits) and block 0 the manufacturer block. */
#define HALTZ_CART_BLOCK_LEN    16u
#define HALTZ_CART_HEADER_BLOCK 1u   /* header: blocks 1 and 2 */
#define HALTZ_CART_BODY_SECTOR  1u   /* body: sectors 1-13, 3 data blocks each */
#define HALTZ_CART_BODY_MAX     624u /* 13 x 48 */
#define HALTZ_CART_SAVE_SECTOR  14u  /* save area: sectors 14-15 */
#define HALTZ_CART_SAVE_LEN     96u  /* 2 x 48 */
#define HALTZ_IMAGE_BODY_MAX    65535u /* a .hzp file in flash (KEY program) */

typedef enum {
    HALTZ_CART_CODE = 0, /* the program is the body */
    HALTZ_CART_KEY = 1,  /* the program lives in S3 flash under key_id */
    HALTZ_CART_DATA = 2, /* saves, settings, a profile: not runnable */
} haltz_cart_type_t;

typedef struct {
    uint8_t type;   /* haltz_cart_type_t */
    char title[HALTZ_CART_TITLE_LEN + 1]; /* terminated */
    uint16_t length;
    uint8_t flags;
    uint32_t key_id;
    uint16_t crc;   /* unpack: the stored CRC; pack ignores it and computes the real one */
} haltz_cart_header_t;

/* Writes the 32-byte header for h and body (h->length bytes) into out, with its CRC.
 * HALTZ_ERR_BAD_VALUE if a field breaks the rules above. */
haltz_err_t haltz_cart_header_pack(const haltz_cart_header_t *h, const uint8_t *body,
                                   uint8_t out[HALTZ_CART_HEADER_LEN]);

/* Reads a header. HALTZ_ERR_BAD_VALUE: not a HALTZ header (magic) or a field breaks the rules;
 * HALTZ_ERR_VERSION: another format. Does not check the CRC (needs the body). */
haltz_err_t haltz_cart_header_unpack(const uint8_t in[HALTZ_CART_HEADER_LEN],
                                     haltz_cart_header_t *h);

/* CRC of a header (bytes 0-29) and body. */
uint16_t haltz_cart_crc(const uint8_t hdr[HALTZ_CART_HEADER_LEN], const uint8_t *body,
                        size_t len);

/* Header, length and CRC together: HALTZ_OK if hdr + body is a whole, intact image.
 * len is the number of body bytes available (at least the header's length). */
haltz_err_t haltz_cart_check(const uint8_t hdr[HALTZ_CART_HEADER_LEN], const uint8_t *body,
                             size_t len, haltz_cart_header_t *h);

/* MIFARE block number for a byte offset in the body (0-623) or the save area (0-95), so the
 * reader can skip the sector trailers. -1 if the offset is past the end. The byte is at
 * offset % 16 in that block. */
int haltz_cart_body_block(unsigned offset);
int haltz_cart_save_block(unsigned offset);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CART_H */
