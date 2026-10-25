#pragma once
#include <cstdint>

namespace saturn {

// Acesso de 8/16 bits dentro de um registrador de 32 bits (big-endian): `offset` é o endereço do acesso.
inline uint32_t lane_shift(uint32_t offset, int bytes) { return bytes == 2 ? ((offset & 2) ? 0u : 16u) : 8 * (3 - (offset & 3)); }
inline uint32_t lane_mask(int bytes) { return bytes == 2 ? 0xFFFFu : 0xFFu; }

inline uint32_t extract_lane(uint32_t word, uint32_t offset, int bytes) {
    return bytes == 4 ? word : (word >> lane_shift(offset, bytes)) & lane_mask(bytes);
}
inline uint32_t insert_lane(uint32_t word, uint32_t offset, int bytes, uint32_t value) {
    if (bytes == 4) return value;
    const uint32_t shift = lane_shift(offset, bytes), mask = lane_mask(bytes);
    return (word & ~(mask << shift)) | ((value & mask) << shift);
}

}  // namespace saturn
