#include <catch2/catch_test_macros.hpp>

#include "sh2_harness.hpp"

using testsupport::Sh2Rig;

namespace {
constexpr uint32_t kVbr = 0x06001000;
constexpr uint32_t kHandler = 0x06002000;

// Prepara VBR e o endereço do handler para o vetor dado.
void set_vector(Sh2Rig& rig, int vector) {
    rig.s().vbr = kVbr;
    rig.bus.write32(kVbr + vector * 4, kHandler);
}

struct IrqDevice : saturn::IoDevice {
    sh2::Cpu* cpu = nullptr;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int, uint32_t) override { cpu->raise_interrupt(5, 0x40); }  // dispara uma interrupção ao ser escrito
};
}  // namespace

TEST_CASE("bra executa o delay slot e salta; as instruções entre o slot e o alvo são puladas") {
    Sh2Rig rig;
    rig.load({0xA002, 0xE105, 0xE206, 0xE307, 0xE408});  // bra +2 ; mov #5,r1 (slot) ; mov #6,r2 ; mov #7,r3 ; alvo: mov #8,r4
    rig.run(2);
    CHECK(rig.r(1) == 5u);
    CHECK(rig.r(2) == 0u);
    CHECK(rig.r(3) == 0u);
    CHECK(rig.r(4) == 8u);
}

TEST_CASE("bsr grava PR = endereço após o slot; bra com deslocamento negativo volta") {
    Sh2Rig rig;
    rig.load({0xB002, Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop});
    rig.run(1);
    CHECK(rig.s().pr == Sh2Rig::kCode + 4);
    CHECK(rig.s().pc == Sh2Rig::kCode + 8);

    Sh2Rig b;
    b.put(Sh2Rig::kCode + 4, {0xAFFE, Sh2Rig::kNop});  // bra -2 : alvo = op + 4 - 4 = o próprio bra
    b.s().pc = Sh2Rig::kCode + 4;
    b.run(1);
    CHECK(b.s().pc == Sh2Rig::kCode + 4);
}

TEST_CASE("braf, bsrf, jmp e jsr") {
    Sh2Rig rig;
    rig.r(1) = 4;
    rig.load({0x0123, Sh2Rig::kNop});  // braf r1 : alvo = op + 4 + 4
    rig.run(1);
    CHECK(rig.s().pc == Sh2Rig::kCode + 8);

    Sh2Rig b;
    b.r(1) = 4;
    b.load({0x0103, Sh2Rig::kNop});  // bsrf r1
    b.run(1);
    CHECK(b.s().pr == Sh2Rig::kCode + 4);
    CHECK(b.s().pc == Sh2Rig::kCode + 8);

    Sh2Rig c;
    c.r(1) = 0x06000100;
    c.load({0x412B, Sh2Rig::kNop});  // jmp @r1
    c.run(1);
    CHECK(c.s().pc == 0x06000100u);

    Sh2Rig d;
    d.r(1) = 0x06000200;
    d.load({0x410B, Sh2Rig::kNop});  // jsr @r1
    d.run(1);
    CHECK(d.s().pr == Sh2Rig::kCode + 4);
    CHECK(d.s().pc == 0x06000200u);
}

TEST_CASE("rts salta para PR depois de executar o delay slot") {
    Sh2Rig rig;
    rig.s().pr = 0x06000300;
    rig.load({0x000B, 0xE109});  // rts ; mov #9,r1 (slot)
    rig.run(1);
    CHECK(rig.r(1) == 9u);
    CHECK(rig.s().pc == 0x06000300u);
}

TEST_CASE("bt/bf (sem slot): saltam só quando a condição vale") {
    struct Case { uint16_t op; bool t; bool taken; };
    for (Case k : {Case{0x8902, true, true}, Case{0x8902, false, false}, Case{0x8B02, false, true}, Case{0x8B02, true, false}}) {
        Sh2Rig rig;
        if (k.t) rig.s().sr |= 1;
        rig.load({k.op});
        rig.run(1);
        CHECK(rig.s().pc == (k.taken ? Sh2Rig::kCode + 8 : Sh2Rig::kCode + 2));
    }
}

TEST_CASE("bt/s e bf/s: com salto executam o slot e saltam; sem salto o slot roda como instrução normal") {
    Sh2Rig taken;
    taken.s().sr |= 1;
    taken.load({0x8D02, 0xE101, 0xE202, 0xE303, 0xE404});  // bt/s +2 ; mov #1,r1 (slot) ; ... ; alvo
    taken.run(1);
    CHECK(taken.r(1) == 1u);
    CHECK(taken.s().pc == Sh2Rig::kCode + 8);

    Sh2Rig not_taken;
    not_taken.load({0x8D02, 0xE101, 0xE202});  // T=0: não salta
    not_taken.run(1);
    CHECK(not_taken.r(1) == 0u);                         // o slot ainda não rodou
    CHECK(not_taken.s().pc == Sh2Rig::kCode + 2);
    not_taken.run(1);
    CHECK(not_taken.r(1) == 1u);                         // rodou uma única vez, como instrução normal

    Sh2Rig bfs;
    bfs.load({0x8F02, 0xE105, 0xE206, 0xE307, 0xE408});  // bf/s com T=0: salta
    bfs.run(1);
    CHECK(bfs.r(1) == 5u);
    CHECK(bfs.s().pc == Sh2Rig::kCode + 8);
}

TEST_CASE("trapa empilha SR e o PC seguinte e vai ao vetor do imediato") {
    Sh2Rig rig;
    set_vector(rig, 0x20);
    rig.s().sr = 0x1F3;
    rig.load({0xC320});  // trapa #0x20
    rig.run(1);
    CHECK(rig.s().pc == kHandler);
    CHECK(rig.r(15) == Sh2Rig::kStack - 8);
    CHECK(rig.bus.read32(rig.r(15)) == Sh2Rig::kCode + 2);       // PC do retorno
    CHECK(rig.bus.read32(rig.r(15) + 4) == 0x1F3u);             // SR salvo
}

TEST_CASE("rte restaura PC e SR (só bits graváveis) e executa o slot") {
    Sh2Rig rig;
    rig.r(15) = 0x06000400;
    rig.bus.write32(0x06000400, 0x06000500);   // PC
    rig.bus.write32(0x06000404, 0xFFFFFFFF);   // SR
    rig.load({0x002B, 0xE109});                // rte ; mov #9,r1
    rig.run(1);
    CHECK(rig.s().pc == 0x06000500u);
    CHECK(rig.s().sr == 0x3F3u);
    CHECK(rig.r(15) == 0x06000408u);
    CHECK(rig.r(1) == 9u);
}

TEST_CASE("instrução ilegal vai ao vetor 4 com o PC da instrução") {
    Sh2Rig rig;
    set_vector(rig, 4);
    rig.load({0xFFFF});
    rig.run(1);
    CHECK(rig.s().pc == kHandler);
    CHECK(rig.bus.read32(rig.r(15)) == Sh2Rig::kCode);
}

TEST_CASE("desvio dentro de um delay slot é instrução ilegal no slot (vetor 6) e não salta") {
    Sh2Rig rig;
    set_vector(rig, 6);
    rig.load({0xA004, 0xA002, Sh2Rig::kNop});  // bra ; bra (no slot!)
    rig.run(1);
    CHECK(rig.s().pc == kHandler);
    CHECK(rig.bus.read32(rig.r(15)) == Sh2Rig::kCode);  // PC salvo = o desvio que tem o slot ilegal
}

TEST_CASE("interrupção respeita a máscara SR.I; NMI (nível 16) ignora a máscara") {
    Sh2Rig rig;
    set_vector(rig, 0x40);
    rig.s().sr = 0x50;                         // I = 5
    rig.cpu.raise_interrupt(5, 0x40);          // nível == I: mascarada
    rig.load({Sh2Rig::kNop, Sh2Rig::kNop});
    rig.run(1);
    CHECK(rig.s().pc == Sh2Rig::kCode + 2);    // não atendida

    rig.cpu.raise_interrupt(6, 0x40);          // nível > I: atendida antes da próxima instrução
    rig.run(1);
    CHECK(rig.s().pc == kHandler);
    CHECK(((rig.s().sr >> 4) & 0xF) == 6u);    // I passa a ser o nível atendido
    CHECK(rig.bus.read32(rig.r(15)) == Sh2Rig::kCode + 2);

    Sh2Rig nmi;
    nmi.s().vbr = kVbr;
    nmi.bus.write32(kVbr + 11 * 4, kHandler);
    nmi.s().sr = 0xF0;                         // I = 15
    nmi.cpu.raise_interrupt(16, 11);
    nmi.load({Sh2Rig::kNop});
    nmi.run(1);
    CHECK(nmi.s().pc == kHandler);
}

TEST_CASE("interrupção levantada durante o delay slot só é atendida depois do desvio") {
    Sh2Rig rig;
    IrqDevice device;
    device.cpu = &rig.cpu;
    rig.bus.map_io(0x05FE0000, 0x100, device);
    set_vector(rig, 0x40);
    rig.s().sr = 0;
    rig.r(2) = 0x05FE0000;
    rig.load({0xA002, 0x2212, Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop});  // bra +2 ; mov.l r1,@r2 (slot dispara a IRQ)
    rig.run(1);
    CHECK(rig.s().pc == Sh2Rig::kCode + 8);    // o desvio terminou antes da interrupção
    rig.run(1);
    CHECK(rig.s().pc == kHandler);
    CHECK(rig.bus.read32(rig.r(15)) == Sh2Rig::kCode + 8);  // retorno = alvo do desvio
}

TEST_CASE("ldc Rm,SR inibe a interrupção por uma instrução") {
    Sh2Rig rig;
    set_vector(rig, 0x40);
    rig.r(1) = 0;
    rig.cpu.raise_interrupt(5, 0x40);
    rig.load({0x410E, Sh2Rig::kNop, Sh2Rig::kNop});  // ldc r1,sr (libera as interrupções) ; nop ; nop
    rig.run(2);
    CHECK(rig.s().pc == Sh2Rig::kCode + 4);          // o nop depois do ldc rodou
    rig.run(1);
    CHECK(rig.s().pc == kHandler);
}

TEST_CASE("sleep para o PC até chegar uma interrupção") {
    Sh2Rig rig;
    set_vector(rig, 0x40);
    rig.s().sr = 0;
    rig.load({0x001B, Sh2Rig::kNop});  // sleep
    rig.run(1);
    CHECK(rig.cpu.sleeping());
    rig.run(3);
    CHECK(rig.s().pc == Sh2Rig::kCode + 2);
    rig.cpu.raise_interrupt(3, 0x40);
    rig.run(1);
    CHECK_FALSE(rig.cpu.sleeping());
    CHECK(rig.s().pc == kHandler);
}

TEST_CASE("callback informa a interrupção atendida") {
    Sh2Rig rig;
    set_vector(rig, 0x41);
    rig.s().sr = 0;
    int taken_level = -1, taken_vector = -1;
    rig.cpu.on_interrupt_taken = [&](int level, int vector) { taken_level = level; taken_vector = vector; };
    rig.cpu.raise_interrupt(7, 0x41);
    rig.load({Sh2Rig::kNop});
    rig.run(1);
    CHECK(taken_level == 7);
    CHECK(taken_vector == 0x41);
}
