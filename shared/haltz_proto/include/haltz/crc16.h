/*
 * CRC-16 used by HALTZ program images and cartridges (docs/vm-v1.md section 8): polynomial
 * 0x1021, init 0xFFFF, no reflection, no final XOR (the catalogue name is CRC-16/CCITT-FALSE,
 * also called CRC-16/IBM-3740). Check value: CRC of ASCII "123456789" = 0x29B1.
 */
#ifndef HALTZ_CRC16_H
#define HALTZ_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALTZ_CRC16_INIT  0xFFFFu
#define HALTZ_CRC16_CHECK 0x29B1u /* haltz_crc16("123456789", 9) */

/* Add bytes to a running CRC. Start with HALTZ_CRC16_INIT. data may be NULL when len is 0. */
uint16_t haltz_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

/* CRC of a whole buffer. */
uint16_t haltz_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_CRC16_H */
