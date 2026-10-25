#include <catch2/catch_test_macros.hpp>

#include "sh2_harness.hpp"

using testsupport::Sh2Rig;

namespace {
// Registradores internos do SH7604, em offsets a partir de 0xFFFFFE00.
constexpr uint32_t kDvsr = 0x100, kDvdnt = 0x104, kDvcr = 0x108, kVcrdiv = 0x10C, kDvdnth = 0x110, kDvdntl = 0x114;
constexpr uint32_t kFtcsr = 0x11, kFrc = 0x12, kOcra = 0x14, kTcr = 0x16;
constexpr uint32_t kIpra = 0xE2;

uint32_t rd32(Sh2Rig& rig, uint32_t offset) { return rig.cpu.onchip().read(offset, 4); }
void wr32(Sh2Rig& rig, uint32_t offset, uint32_t value) { rig.cpu.onchip().write(offset, 4, value); }
}  // namespace

TEST_CASE("DIVU 32/32 com sinal: escrever DVDNT divide; quociente em DVDNT, resto em DVDNTH") {
    Sh2Rig rig;
    wr32(rig, kDvsr, 7);
    wr32(rig, kDvdnt, 100);
    CHECK(rd32(rig, kDvdnt) == 14u);
    CHECK(rd32(rig, kDvdnth) == 2u);

    wr32(rig, kDvdnt, 0xFFFFFF9C);          // -100 / 7 = -14, resto -2
    CHECK(rd32(rig, kDvdnt) == 0xFFFFFFF2u);
    CHECK(rd32(rig, kDvdnth) == 0xFFFFFFFEu);
}

TEST_CASE("DIVU 64/32: DVDNTH:DVDNTL dividido por DVSR; escrever DVDNTL dispara") {
    Sh2Rig rig;
    wr32(rig, kDvsr, 3);
    wr32(rig, kDvdnth, 1);
    wr32(rig, kDvdntl, 0);                  // 2^32 / 3
    CHECK(rd32(rig, kDvdntl) == 0x55555555u);
    CHECK(rd32(rig, kDvdnth) == 1u);
    CHECK(rd32(rig, kDvdnt) == 0x55555555u);   // DVDNT é o mesmo quociente
}

TEST_CASE("DIVU: divisão por zero sinaliza OVF e satura; com OVFIE gera a interrupção do VCRDIV") {
    Sh2Rig rig;
    wr32(rig, kDvsr, 0);
    wr32(rig, kDvdnt, 5);
    CHECK((rd32(rig, kDvcr) & 1) == 1u);       // OVF
    CHECK(rd32(rig, kDvdnt) == 0x7FFFFFFFu);   // positivo ÷ 0 satura no máximo
    wr32(rig, kDvdnt, 0xFFFFFFFB);
    CHECK(rd32(rig, kDvdnt) == 0x80000000u);   // negativo ÷ 0 satura no mínimo

    Sh2Rig irq;
    irq.s().vbr = 0x06001000;
    irq.bus.write32(0x06001000 + 0x70 * 4, 0x06002000);
    irq.s().sr = 0;
    wr32(irq, kVcrdiv, 0x70);
    irq.cpu.onchip().write(kIpra, 2, 0x5000);   // prioridade 5 para a DIVU
    wr32(irq, kDvcr, 2);                        // OVFIE
    wr32(irq, kDvsr, 0);
    wr32(irq, kDvdnt, 1);
    irq.load({Sh2Rig::kNop});
    irq.run(1);
    CHECK(irq.s().pc == 0x06002000u);
    CHECK(((irq.s().sr >> 4) & 0xF) == 5u);
}

TEST_CASE("o CPU encaminha acessos a 0xFFFFFExx/0xFFFFFFxx aos registradores internos") {
    Sh2Rig rig;
    rig.r(1) = 0xFFFFFF00; rig.r(2) = 9; rig.r(3) = 0xFFFFFF04; rig.r(4) = 81;
    rig.load({0x2122, 0x2342, 0x6532});   // mov.l r2,@r1 (DVSR=9) ; mov.l r4,@r3 (DVDNT=81) ; mov.l @r3,r5
    rig.run(3);
    CHECK(rig.r(5) == 9u);
}

TEST_CASE("FRT: FRC conta ciclos/8 (CKS=0), marca OVF no estouro e OCFA na comparação") {
    Sh2Rig rig;
    rig.cpu.onchip().write(kTcr, 1, 0);        // φ/8
    rig.cpu.onchip().advance(80);
    CHECK(rig.cpu.onchip().read(kFrc, 2) == 10u);

    rig.cpu.onchip().write(kOcra, 2, 15);
    rig.cpu.onchip().advance(8 * 5);           // FRC: 10 -> 15
    CHECK((rig.cpu.onchip().read(kFtcsr, 1) & 0x08) != 0);   // OCFA

    rig.cpu.onchip().write(kFrc, 2, 0xFFFF);
    rig.cpu.onchip().advance(8);
    CHECK(rig.cpu.onchip().read(kFrc, 2) == 0u);
    CHECK((rig.cpu.onchip().read(kFtcsr, 1) & 0x02) != 0);   // OVF

    rig.cpu.onchip().write(kFtcsr, 1, 0x00);                 // escrever 0 limpa os flags
    CHECK(rig.cpu.onchip().read(kFtcsr, 1) == 0u);
}

TEST_CASE("áreas de cache do SH-2: purga (0x4...) e array de endereços (0x6...) são ignoradas sem contar como não mapeadas") {
    Sh2Rig rig;
    rig.r(1) = 0x40000010; rig.r(2) = 0x60000020; rig.r(3) = 0x1234;
    rig.load({0x2132, 0x2232, 0x6422});   // mov.l r3,@r1 ; mov.l r3,@r2 ; mov.l @r2,r4
    rig.r(4) = 0xFFFF;
    rig.run(3);
    CHECK(rig.bus.unmapped_count() == 0u);
    CHECK(rig.r(4) == 0u);
}

TEST_CASE("array de dados do cache (0xC0000000, 4 KiB) funciona como RAM rápida") {
    Sh2Rig rig;
    rig.r(1) = 0xC0000100; rig.r(2) = 0xCAFEBABE;
    rig.load({0x2122, 0x6312});   // mov.l r2,@r1 ; mov.l @r1,r3
    rig.run(2);
    CHECK(rig.r(3) == 0xCAFEBABEu);
    CHECK(rig.bus.unmapped_count() == 0u);

    Sh2Rig b;                      // espelhado a cada 4 KiB
    b.r(1) = 0xC0000100; b.r(2) = 0x11223344; b.r(4) = 0xC0001100;
    b.load({0x2122, 0x6342});     // mov.l r2,@r1 ; mov.l @r4,r3
    b.run(2);
    CHECK(b.r(3) == 0x11223344u);
}
