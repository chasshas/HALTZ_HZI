/*
 * Little-endian byte helpers for pack/unpack functions. Private to haltz_proto (src/ only).
 * Callers check buffer sizes first; these never check.
 */
#ifndef HALTZ_WIRE_H
#define HALTZ_WIRE_H

#include <stddef.h>
#include <stdint.h>

static inline void put_u8(uint8_t *buf, size_t *pos, uint8_t v) {
    buf[(*pos)++] = v;
}

static inline void put_u16le(uint8_t *buf, size_t *pos, uint16_t v) {
    buf[(*pos)++] = (uint8_t)(v & 0xFFu);
    buf[(*pos)++] = (uint8_t)(v >> 8);
}

static inline void put_i16le(uint8_t *buf, size_t *pos, int16_t v) {
    put_u16le(buf, pos, (uint16_t)v); /* two's complement */
}

static inline void put_u32le(uint8_t *buf, size_t *pos, uint32_t v) {
    put_u16le(buf, pos, (uint16_t)(v & 0xFFFFu));
    put_u16le(buf, pos, (uint16_t)(v >> 16));
}

static inline uint8_t get_u8(const uint8_t *buf, size_t *pos) {
    return buf[(*pos)++];
}

static inline uint16_t get_u16le(const uint8_t *buf, size_t *pos) {
    uint16_t v = (uint16_t)buf[*pos] | (uint16_t)((uint16_t)buf[*pos + 1] << 8);
    *pos += 2;
    return v;
}

static inline int16_t get_i16le(const uint8_t *buf, size_t *pos) {
    uint16_t u = get_u16le(buf, pos);
    /* Portable two's complement conversion (no implementation-defined cast). */
    return (u & 0x8000u) ? (int16_t)(-(int32_t)(0x10000u - u)) : (int16_t)u;
}

static inline uint32_t get_u32le(const uint8_t *buf, size_t *pos) {
    uint32_t lo = get_u16le(buf, pos);
    uint32_t hi = get_u16le(buf, pos);
    return lo | (hi << 16);
}

#endif /* HALTZ_WIRE_H */
