#include <catch2/catch_test_macros.hpp>

#include "sh2_harness.hpp"

using testsupport::Sh2Rig;

// Opcodes calculados à mão a partir do manual SH-1/SH-2 (n = registrador destino, m = origem, d/i = imediatos).

TEST_CASE("nop só avança o PC") {
    Sh2Rig rig;
    rig.r(1) = 7;
    rig.load({Sh2Rig::kNop});
    auto cycles = rig.cpu.step();
    CHECK(rig.s().pc == Sh2Rig::kCode + 2);
    CHECK(rig.r(1) == 7u);
    CHECK(cycles == 1u);
}

TEST_CASE("mov.w @Rm+ com endereço ímpar causa erro de endereço (vetor 9) em vez de ler") {
    Sh2Rig rig;
    rig.s().vbr = 0x06001000;
    rig.bus.write32(0x06001000 + 9 * 4, 0x06002000);  // handler do erro de endereço
    rig.r(1) = 0x06000205; rig.r(3) = 0x1111;
    rig.load({0x6315});  // mov.w @r1+,r3
    rig.run(1);
    CHECK(rig.s().pc == 0x06002000u);
    CHECK(rig.r(3) == 0x1111u);                 // destino intocado
    CHECK(rig.bus.read32(rig.r(15)) == Sh2Rig::kCode);  // PC salvo = instrução que falhou
}

TEST_CASE("mov #imm,Rn estende o sinal") {
    Sh2Rig rig;
    rig.load({0xE1FF, 0xE27F, 0xE300});  // r1=-1, r2=0x7F, r3=0
    rig.run(3);
    CHECK(rig.r(1) == 0xFFFFFFFFu);
    CHECK(rig.r(2) == 0x7Fu);
    CHECK(rig.r(3) == 0u);
}

TEST_CASE("mov Rm,Rn copia o registrador") {
    Sh2Rig rig;
    rig.r(2) = 0xDEADBEEF;
    rig.load({0x6123});  // mov r2,r1
    rig.run(1);
    CHECK(rig.r(1) == 0xDEADBEEFu);
}

TEST_CASE("mov.b/w/l Rm,@Rn e @Rm,Rn: tamanhos e extensão de sinal na leitura") {
    Sh2Rig rig;
    rig.r(1) = 0x06000100;

    rig.r(2) = 0x11223384;
    rig.load({0x2120, 0x6310});  // mov.b r2,@r1 ; mov.b @r1,r3
    rig.run(2);
    CHECK(rig.bus.read8(0x06000100) == 0x84);
    CHECK(rig.r(3) == 0xFFFFFF84u);

    rig.cpu.reset(Sh2Rig::kCode, Sh2Rig::kStack);
    rig.r(1) = 0x06000100; rig.r(2) = 0x00008001;
    rig.load({0x2121, 0x6311});  // mov.w r2,@r1 ; mov.w @r1,r3
    rig.run(2);
    CHECK(rig.bus.read16(0x06000100) == 0x8001);
    CHECK(rig.r(3) == 0xFFFF8001u);

    rig.cpu.reset(Sh2Rig::kCode, Sh2Rig::kStack);
    rig.r(1) = 0x06000100; rig.r(2) = 0x89ABCDEF;
    rig.load({0x2122, 0x6312});  // mov.l r2,@r1 ; mov.l @r1,r3
    rig.run(2);
    CHECK(rig.r(3) == 0x89ABCDEFu);
}

TEST_CASE("pré-decremento na escrita e pós-incremento na leitura") {
    Sh2Rig rig;
    rig.r(1) = 0x06000100; rig.r(2) = 0xCAFEBABE;
    rig.load({0x2126});  // mov.l r2,@-r1
    rig.run(1);
    CHECK(rig.r(1) == 0x060000FCu);
    CHECK(rig.bus.read32(0x060000FC) == 0xCAFEBABEu);

    Sh2Rig b;
    b.bus.write32(0x06000200, 0x01020304);
    b.r(1) = 0x06000200;
    b.load({0x6316, 0x6315, 0x6314});  // mov.l @r1+,r3 ; mov.w @r1+,r3 ; mov.b @r1+,r3 (ordem que mantém o alinhamento)
    b.run(1);
    CHECK(b.r(3) == 0x01020304u);
    CHECK(b.r(1) == 0x06000204u);
    b.run(1);
    CHECK(b.r(1) == 0x06000206u);  // +2
    b.run(1);
    CHECK(b.r(1) == 0x06000207u);  // +1
}

TEST_CASE("mov.l @Rm+,Rm: o valor lido prevalece sobre o pós-incremento") {
    Sh2Rig rig;
    rig.bus.write32(0x06000300, 0x12345678);
    rig.r(1) = 0x06000300;
    rig.load({0x6116});  // mov.l @r1+,r1
    rig.run(1);
    CHECK(rig.r(1) == 0x12345678u);
}

TEST_CASE("deslocamento: mov.l/.w/.b com @(disp,Rn) e @(disp,Rm)") {
    Sh2Rig rig;
    rig.r(1) = 0x06000400; rig.r(2) = 0xA1B2C3D4;
    rig.load({0x1123, 0x5312});  // mov.l r2,@(3,r1) [+12] ; mov.l @(2,r1),r3 [+8]
    rig.run(1);
    CHECK(rig.bus.read32(0x0600040C) == 0xA1B2C3D4u);
    rig.bus.write32(0x06000408, 0x0BADF00D);
    rig.run(1);
    CHECK(rig.r(3) == 0x0BADF00Du);

    Sh2Rig b;
    b.r(1) = 0x06000500; b.r(0) = 0x7F81;
    b.load({0x8015, 0x8115, 0x8415, 0x8515});  // mov.b r0,@(5,r1) ; mov.w r0,@(5,r1)[+10] ; mov.b @(5,r1),r0 ; mov.w @(5,r1),r0
    b.run(1);
    CHECK(b.bus.read8(0x06000505) == 0x81);
    b.run(1);
    CHECK(b.bus.read16(0x0600050A) == 0x7F81);
    b.r(0) = 0;
    b.run(1);
    CHECK(b.r(0) == 0xFFFFFF81u);   // byte estendido em sinal
    b.run(1);
    b.bus.write16(0x0600050A, 0x8002);
    b.cpu.reset(Sh2Rig::kCode, Sh2Rig::kStack); b.r(1) = 0x06000500;
    b.put(Sh2Rig::kCode, {0x8515});
    b.run(1);
    CHECK(b.r(0) == 0xFFFF8002u);   // word estendido em sinal
}

TEST_CASE("indexado por R0: mov.b/w/l @(R0,Rn)") {
    Sh2Rig rig;
    rig.r(0) = 0x10; rig.r(1) = 0x06000600; rig.r(2) = 0x55667788;
    rig.load({0x0126, 0x031E});  // mov.l r2,@(r0,r1) ; mov.l @(r0,r1),r3
    rig.run(2);
    CHECK(rig.bus.read32(0x06000610) == 0x55667788u);
    CHECK(rig.r(3) == 0x55667788u);

    Sh2Rig b;
    b.r(0) = 4; b.r(1) = 0x06000700; b.r(2) = 0x80;
    b.load({0x0124, 0x031C});  // mov.b r2,@(r0,r1) ; mov.b @(r0,r1),r3
    b.run(2);
    CHECK(b.r(3) == 0xFFFFFF80u);
}

TEST_CASE("relativo ao GBR: mov.l/.b @(disp,GBR),R0 e R0,@(disp,GBR)") {
    Sh2Rig rig;
    rig.s().gbr = 0x06000800; rig.r(0) = 0xFEDCBA98;
    rig.load({0xC202, 0xC602});  // mov.l r0,@(2,gbr) [+8] ; mov.l @(2,gbr),r0
    rig.run(1);
    CHECK(rig.bus.read32(0x06000808) == 0xFEDCBA98u);
    rig.r(0) = 0;
    rig.run(1);
    CHECK(rig.r(0) == 0xFEDCBA98u);

    Sh2Rig b;
    b.s().gbr = 0x06000900; b.r(0) = 0x7B;
    b.load({0xC003, 0xC403});  // mov.b r0,@(3,gbr) ; mov.b @(3,gbr),r0
    b.run(2);
    CHECK(b.bus.read8(0x06000903) == 0x7B);
    CHECK(b.r(0) == 0x7Bu);
}

TEST_CASE("PC-relativo: mov.w, mov.l (alinhado a 4) e mova") {
    Sh2Rig rig;
    rig.load({0x9102, Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop, 0x8000});  // mov.w @(2,pc),r1 -> literal em 0x...08
    rig.run(1);
    CHECK(rig.r(1) == 0xFFFF8000u);

    Sh2Rig b;
    b.load({Sh2Rig::kNop, 0xD101, Sh2Rig::kNop, Sh2Rig::kNop, 0x1234, 0x5678});  // mov.l @(1,pc),r1 em 0x...02
    b.run(2);
    CHECK(b.r(1) == 0x12345678u);

    Sh2Rig c;
    c.load({0xC701});  // mova @(1,pc),r0 em 0x...00 -> (0x04 & ~3) + 4
    c.run(1);
    CHECK(c.r(0) == 0x06000008u);
    Sh2Rig d;
    d.load({Sh2Rig::kNop, 0xC700});  // mova @(0,pc),r0 em 0x...02 -> (0x06 & ~3) + 0
    d.run(2);
    CHECK(d.r(0) == 0x06000004u);
}

TEST_CASE("movt, swap.b, swap.w e xtrct") {
    Sh2Rig rig;
    rig.s().sr |= 1;
    rig.load({0x0129});  // movt r1
    rig.run(1);
    CHECK(rig.r(1) == 1u);

    Sh2Rig b;
    b.r(2) = 0x11223344;
    b.load({0x6128, 0x6329, 0x0000});  // swap.b r2,r1 ; swap.w r2,r3
    b.run(2);
    CHECK(b.r(1) == 0x11224433u);
    CHECK(b.r(3) == 0x33441122u);

    Sh2Rig c;
    c.r(1) = 0xAAAABBBB; c.r(2) = 0xCCCCDDDD;
    c.load({0x212D});  // xtrct r2,r1
    c.run(1);
    CHECK(c.r(1) == 0xDDDDAAAAu);
}

TEST_CASE("ldc/stc/lds/sts: SR só guarda os bits graváveis (0x3F3)") {
    Sh2Rig rig;
    rig.r(1) = 0xFFFFFFFF;
    rig.load({0x410E, 0x411E, 0x412E, 0x0202, 0x0312, 0x0422});  // ldc r1,sr/gbr/vbr ; stc sr,r2 ; stc gbr,r3 ; stc vbr,r4
    rig.run(6);
    CHECK(rig.s().sr == 0x3F3u);
    CHECK(rig.r(2) == 0x3F3u);
    CHECK(rig.r(3) == 0xFFFFFFFFu);
    CHECK(rig.r(4) == 0xFFFFFFFFu);

    Sh2Rig b;
    b.r(1) = 0x01020304;
    b.load({0x410A, 0x411A, 0x412A, 0x020A, 0x031A, 0x042A});  // lds r1,mach/macl/pr ; sts mach,r2 ; sts macl,r3 ; sts pr,r4
    b.run(6);
    CHECK(b.s().mach == 0x01020304u);
    CHECK(b.r(2) == 0x01020304u);
    CHECK(b.r(3) == 0x01020304u);
    CHECK(b.r(4) == 0x01020304u);
}

TEST_CASE("ldc.l/lds.l @Rm+ e stc.l/sts.l @-Rn") {
    Sh2Rig rig;
    rig.bus.write32(0x06000A00, 0x0000FFFF);
    rig.bus.write32(0x06000A04, 0x12345678);
    rig.r(1) = 0x06000A00;
    rig.load({0x4107, 0x4116});  // ldc.l @r1+,sr ; lds.l @r1+,macl
    rig.run(2);
    CHECK(rig.s().sr == 0x3F3u);
    CHECK(rig.s().macl == 0x12345678u);
    CHECK(rig.r(1) == 0x06000A08u);

    Sh2Rig b;
    b.s().gbr = 0xAABBCCDD; b.s().pr = 0x11111111;
    b.r(1) = 0x06000B10;
    b.load({0x4113, 0x4122});  // stc.l gbr,@-r1 ; sts.l pr,@-r1
    b.run(2);
    CHECK(b.r(1) == 0x06000B08u);
    CHECK(b.bus.read32(0x06000B0C) == 0xAABBCCDDu);
    CHECK(b.bus.read32(0x06000B08) == 0x11111111u);
}
