#pragma once
#include <initializer_list>
#include <span>

#include "saturn/bus.hpp"
#include "sh2/cpu.hpp"

namespace testsupport {

// Máquina mínima para testar o SH-2: RAM padrão do Saturn, CPU resetada em 0x06000000 com pilha em 0x060FFFF0.
struct Sh2Rig {
    static constexpr uint32_t kCode = 0x06000000;
    static constexpr uint32_t kStack = 0x060FFFF0;
    static constexpr uint16_t kNop = 0x0009;

    saturn::Bus bus;
    sh2::Cpu cpu{bus, 0};

    Sh2Rig() {
        saturn::map_standard_memory(bus);
        cpu.reset(kCode, kStack);
    }

    // Grava palavras de 16 bits consecutivas a partir de `address`.
    void put(uint32_t address, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) { bus.write16(address, w); address += 2; }
    }
    void put(uint32_t address, std::span<const uint16_t> words) {
        for (uint16_t w : words) { bus.write16(address, w); address += 2; }
    }
    void load(std::initializer_list<uint16_t> ops) { put(kCode, ops); }
    void load(std::span<const uint16_t> ops) { put(kCode, ops); }
    void run(int steps) { for (int i = 0; i < steps; ++i) cpu.step(); }

    uint32_t& r(int i) { return cpu.state().r[i]; }
    sh2::Sh2State& s() { return cpu.state(); }
    bool t() { return cpu.state().sr & sh2::kSrT; }
};

// Opcode com os campos n (bits 8–11) e m (bits 4–7).
constexpr uint16_t nm(uint16_t base, int n, int m) { return static_cast<uint16_t>(base | (n << 8) | (m << 4)); }
constexpr uint16_t n_(uint16_t base, int n) { return static_cast<uint16_t>(base | (n << 8)); }

}  // namespace testsupport
