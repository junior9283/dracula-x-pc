#pragma once
#include <cstdint>

#include "saturn/bus.hpp"

namespace saturn {

// B-bus (SCSP, VDP1, VDP2: 0x05A00000–0x05FFFFFF): 16 bits de largura. Usado pelos DMAs do SCU e do DSP.
constexpr bool is_b_bus(uint32_t address) { return address >= 0x05A0'0000 && address < 0x0600'0000; }

// Um longword por DMA vira duas escritas de 16 bits, cada uma somando o passo; devolve o endereço da segunda.
inline uint32_t write_b_bus_long(Bus& bus, uint32_t address, uint32_t value, uint32_t step) {
    bus.write16(address, static_cast<uint16_t>(value >> 16));
    address += step;
    bus.write16(address, static_cast<uint16_t>(value));
    return address;
}

}  // namespace saturn
