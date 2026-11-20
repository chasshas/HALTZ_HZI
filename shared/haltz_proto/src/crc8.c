#include "haltz/crc8.h"

uint8_t haltz_crc8_update(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int bit = 0; bit < 8; bit++) {
        if (crc & 0x80u) {
            crc = (uint8_t)(((unsigned)crc << 1) ^ 0x07u);
        } else {
            crc = (uint8_t)((unsigned)crc << 1);
        }
    }
    return crc;
}

uint8_t haltz_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = HALTZ_CRC8_INIT;
    for (size_t i = 0; i < len; i++) {
        crc = haltz_crc8_update(crc, data[i]);
    }
    return crc;
}
