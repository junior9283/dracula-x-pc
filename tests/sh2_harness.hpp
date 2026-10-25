#pragma once
#include <initializer_list>

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
    void load(std::initializer_list<uint16_t> ops) { put(kCode, ops); }
    void run(int steps) { for (int i = 0; i < steps; ++i) cpu.step(); }

    uint32_t& r(int i) { return cpu.state().r[i]; }
    sh2::Sh2State& s() { return cpu.state(); }
};

}  // namespace testsupport
