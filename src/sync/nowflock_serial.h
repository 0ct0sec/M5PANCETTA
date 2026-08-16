/**
 * NOWFLOCK portable serialization helpers.
 *
 * little endian. no host-endian roulette.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NowFlockSerial {

inline void writeU8(uint8_t* out, uint8_t v) {
    out[0] = v;
}

inline void writeU16(uint8_t* out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

inline void writeU32(uint8_t* out, uint32_t v) {
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

inline void writeI32(uint8_t* out, int32_t v) {
    writeU32(out, (uint32_t)v);
}

inline uint8_t readU8(const uint8_t* in) {
    return in[0];
}

inline uint16_t readU16(const uint8_t* in) {
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

inline uint32_t readU32(const uint8_t* in) {
    return (uint32_t)in[0]
        | ((uint32_t)in[1] << 8)
        | ((uint32_t)in[2] << 16)
        | ((uint32_t)in[3] << 24);
}

inline int32_t readI32(const uint8_t* in) {
    return (int32_t)readU32(in);
}

inline bool canRead(size_t offset, size_t need, size_t len) {
    return offset <= len && need <= (len - offset);
}

inline uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data ? data[i] : 0;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace NowFlockSerial
