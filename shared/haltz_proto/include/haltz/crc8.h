/*
 * CRC-8 used by HALTZ frames: polynomial 0x07, init 0x00, no reflection, no final XOR
 * (the catalogue name is CRC-8/SMBUS). Check value: CRC of ASCII "123456789" = 0xF4.
 */
#ifndef HALTZ_CRC8_H
#define HALTZ_CRC8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALTZ_CRC8_INIT  0x00u
#define HALTZ_CRC8_CHECK 0xF4u /* haltz_crc8("123456789", 9) */

/* Add one byte to a running CRC. Start with HALTZ_CRC8_INIT. */
uint8_t haltz_crc8_update(uint8_t crc, uint8_t byte);

/* CRC of a whole buffer. data may be NULL when len is 0. */
uint8_t haltz_crc8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CRC8_H */
