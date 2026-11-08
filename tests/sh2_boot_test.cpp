// SH-2: detalhes usados pelo BIOS real no boot (pino MASTER do BCR1 e NMI).
#include <catch2/catch_test_macros.hpp>

#include "sh2_harness.hpp"

using testsupport::Sh2Rig;

namespace {
constexpr uint32_t kBcr1 = 0x1E0, kBcr1Low = 0x1E2;   // BCR1 em 0xFFFFFFE0; o bit 15 está no byte 0x1E2
constexpr uint32_t kHandler = 0x06002000, kVbr = 0x06001000;

// Cpu de id `id` (0 = mestre, 1 = escravo) sozinha no barramento.
struct IdRig {
    saturn::Bus bus;
    sh2::Cpu cpu;
    explicit IdRig(int id) : cpu(bus, id) { saturn::map_standard_memory(bus); }
};
}  // namespace

TEST_CASE("BCR1 bit 15 (MASTER) lê 0 no mestre e 1 no escravo") {
    IdRig master(0), slave(1);
    CHECK((master.cpu.onchip().read(kBcr1, 4) & 0x8000) == 0u);
    CHECK((slave.cpu.onchip().read(kBcr1, 4) & 0x8000) == 0x8000u);
    CHECK((master.cpu.onchip().read(kBcr1Low, 1) & 0x80) == 0u);
    CHECK((slave.cpu.onchip().read(kBcr1Low, 1) & 0x80) == 0x80u);
}

TEST_CASE("BCR1 bit 15 não muda com escritas: o pino dita o valor") {
    IdRig master(0), slave(1);
    for (uint32_t value : {0xFFFFFFFFu, 0x00000000u, 0x0000A55Au}) {
        master.cpu.onchip().write(kBcr1, 4, value);
        slave.cpu.onchip().write(kBcr1, 4, value);
        CHECK((master.cpu.onchip().read(kBcr1, 4) & 0x8000) == 0u);
        CHECK((slave.cpu.onchip().read(kBcr1, 4) & 0x8000) == 0x8000u);
    }
    master.cpu.onchip().write(kBcr1Low, 1, 0xFF);
    slave.cpu.onchip().write(kBcr1Low, 1, 0x00);
    CHECK((master.cpu.onchip().read(kBcr1Low, 1) & 0x80) == 0u);
    CHECK((slave.cpu.onchip().read(kBcr1Low, 1) & 0x80) == 0x80u);
}

TEST_CASE("BCR1: os demais bits do registro seguem a escrita") {
    IdRig slave(1);
    slave.cpu.onchip().write(kBcr1, 4, 0x00001234);
    CHECK((slave.cpu.onchip().read(kBcr1, 4) & 0x7FFF) == 0x1234u);
    CHECK((slave.cpu.onchip().read(kBcr1, 4) & 0x8000) == 0x8000u);
}

TEST_CASE("NMI (nível 16, vetor 11) é atendido mesmo com SR.I = 15") {
    Sh2Rig rig;
    rig.load({Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop});
    rig.s().vbr = kVbr;
    rig.bus.write32(kVbr + sh2::Cpu::kNmiVector * 4, kHandler);
    REQUIRE(((rig.s().sr >> 4) & 0xF) == 15u);

    int level = -1, vector = -1;
    rig.cpu.on_interrupt_taken = [&](int l, int v) { level = l; vector = v; };
    rig.cpu.raise_interrupt(sh2::Cpu::kNmiLevel, sh2::Cpu::kNmiVector);
    rig.cpu.step();

    CHECK(level == sh2::Cpu::kNmiLevel);
    CHECK(vector == sh2::Cpu::kNmiVector);
    CHECK(rig.s().pc == kHandler);
    CHECK(((rig.s().sr >> 4) & 0xF) == 15u);              // a máscara fica em 15
    CHECK(rig.s().r[15] == Sh2Rig::kStack - 8);           // SR e PC empilhados
    CHECK(rig.bus.read32(rig.s().r[15]) == Sh2Rig::kCode);
}

TEST_CASE("sem NMI, uma interrupção de nível 15 com SR.I = 15 continua mascarada") {
    Sh2Rig rig;
    rig.load({Sh2Rig::kNop, Sh2Rig::kNop});
    rig.s().vbr = kVbr;
    rig.bus.write32(kVbr + 0x40 * 4, kHandler);
    bool taken = false;
    rig.cpu.on_interrupt_taken = [&](int, int) { taken = true; };
    rig.cpu.raise_interrupt(15, 0x40);
    rig.cpu.step();
    CHECK_FALSE(taken);
    CHECK(rig.s().pc == Sh2Rig::kCode + 2);
}

TEST_CASE("NMI acorda a CPU em SLEEP com SR.I = 15") {
    Sh2Rig rig;
    rig.load({0x001B});                                   // SLEEP
    rig.s().vbr = kVbr;
    rig.bus.write32(kVbr + sh2::Cpu::kNmiVector * 4, kHandler);
    rig.cpu.step();                                       // dorme
    rig.cpu.step();
    CHECK(rig.s().pc != kHandler);
    rig.cpu.raise_interrupt(sh2::Cpu::kNmiLevel, sh2::Cpu::kNmiVector);
    rig.cpu.step();
    CHECK(rig.s().pc == kHandler);
}
