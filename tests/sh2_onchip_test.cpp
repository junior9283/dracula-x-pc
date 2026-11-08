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

TEST_CASE("FRT: sinal de captura de entrada levanta ICF e copia o FRC para o ICR; ICF zera ao escrever 0") {
    Sh2Rig rig;
    rig.cpu.onchip().write(kFrc, 2, 0x1234);
    rig.cpu.onchip().signal_input_capture();
    CHECK((rig.cpu.onchip().read(kFtcsr, 1) & 0x80) == 0x80);
    CHECK(rig.cpu.onchip().read(0x18, 2) == 0x1234u);       // ICR
    rig.cpu.onchip().write(kFtcsr, 1, 0x00);
    CHECK((rig.cpu.onchip().read(kFtcsr, 1) & 0x80) == 0);
}

namespace {
constexpr uint32_t kSar0 = 0x180, kDar0 = 0x184, kTcr0 = 0x188, kChcr0 = 0x18C, kVcrdma0 = 0x1A0, kDmaor = 0x1B0;
}

TEST_CASE("DMAC canal 0 em pedido automático: copia TCR palavras, avança SAR/DAR, zera TCR e liga TE") {
    Sh2Rig rig;
    for (uint32_t i = 0; i < 8; ++i) rig.bus.write16(0x06010000 + 2 * i, static_cast<uint16_t>(0x1100 + i));
    wr32(rig, kSar0, 0x06010000);
    wr32(rig, kDar0, 0x06020000);
    wr32(rig, kTcr0, 8);
    wr32(rig, kDmaor, 1);
    wr32(rig, kChcr0, 0x5600);                       // DM/SM incrementam, palavra, pedido automático; DE ainda desligado
    CHECK(rig.bus.read16(0x06020000) == 0u);         // nada copiado antes de DE
    wr32(rig, kChcr0, 0x5601);                       // liga DE

    for (uint32_t i = 0; i < 8; ++i) CHECK(rig.bus.read16(0x06020000 + 2 * i) == 0x1100 + i);
    CHECK(rd32(rig, kTcr0) == 0u);
    CHECK(rd32(rig, kSar0) == 0x06010010u);
    CHECK(rd32(rig, kDar0) == 0x06020010u);
    CHECK((rd32(rig, kChcr0) & 2) == 2u);            // TE
    wr32(rig, kChcr0, rd32(rig, kChcr0) & ~3u);      // o software zera TE (e DE) escrevendo 0
    CHECK((rd32(rig, kChcr0) & 2) == 0u);
}

TEST_CASE("DMAC: sem DME em DMAOR a transferência não acontece; com IE, TE gera a interrupção do VCRDMA0") {
    Sh2Rig rig;
    rig.bus.write32(0x06010000, 0xCAFEBABE);
    wr32(rig, kSar0, 0x06010000);
    wr32(rig, kDar0, 0x06020000);
    wr32(rig, kTcr0, 1);
    wr32(rig, kChcr0, 0x5A05);                       // DM/SM incrementam, longword, pedido automático, IE, DE
    CHECK(rig.bus.read32(0x06020000) == 0u);         // DME desligado

    rig.s().vbr = 0x06001000;
    rig.bus.write32(0x06001000 + 0x50 * 4, 0x06002000);
    rig.s().sr = 0;
    wr32(rig, kVcrdma0, 0x50);
    rig.cpu.onchip().write(kIpra, 2, 0x0500);        // DMAC no nível 5
    wr32(rig, kDmaor, 1);                            // ligar DME com DE já ligado dispara a transferência
    CHECK(rig.bus.read32(0x06020000) == 0xCAFEBABEu);
    rig.load({Sh2Rig::kNop});
    rig.run(1);                                      // aceita a interrupção
    CHECK(rig.s().pc == 0x06002000u);
}
