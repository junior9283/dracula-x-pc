#include <catch2/catch_test_macros.hpp>

#include "sh2_harness.hpp"

using testsupport::Sh2Rig;

namespace {
// Opcode com os campos n (bits 8–11) e m (bits 4–7).
constexpr uint16_t nm(uint16_t base, int n, int m) { return static_cast<uint16_t>(base | (n << 8) | (m << 4)); }
constexpr uint16_t n_(uint16_t base, int n) { return static_cast<uint16_t>(base | (n << 8)); }
bool t(Sh2Rig& rig) { return rig.s().sr & 1; }
}  // namespace

TEST_CASE("add e add #imm (imediato com sinal)") {
    Sh2Rig rig;
    rig.r(1) = 5; rig.r(2) = 7; rig.r(3) = 10;
    rig.load({nm(0x300C, 1, 2), 0x7300 | 0xFE});  // add r2,r1 ; add #-2,r3
    rig.run(2);
    CHECK(rig.r(1) == 12u);
    CHECK(rig.r(3) == 8u);
}

TEST_CASE("addc: soma com T e gera carry") {
    Sh2Rig rig;
    rig.r(1) = 0xFFFFFFFF; rig.r(2) = 0;
    rig.s().sr |= 1;
    rig.load({nm(0x300E, 1, 2)});
    rig.run(1);
    CHECK(rig.r(1) == 0u);
    CHECK(t(rig));

    Sh2Rig b;
    b.r(1) = 1; b.r(2) = 2;
    b.load({nm(0x300E, 1, 2)});
    b.run(1);
    CHECK(b.r(1) == 3u);
    CHECK_FALSE(t(b));
}

TEST_CASE("addv: T indica overflow com sinal") {
    Sh2Rig rig;
    rig.r(1) = 0x7FFFFFFF; rig.r(2) = 1;
    rig.load({nm(0x300F, 1, 2)});
    rig.run(1);
    CHECK(rig.r(1) == 0x80000000u);
    CHECK(t(rig));

    Sh2Rig b;
    b.r(1) = 1; b.r(2) = 1;
    b.load({nm(0x300F, 1, 2)});
    b.run(1);
    CHECK(b.r(1) == 2u);
    CHECK_FALSE(t(b));
}

TEST_CASE("sub, subc (borrow) e subv (overflow)") {
    Sh2Rig rig;
    rig.r(1) = 10; rig.r(2) = 3;
    rig.load({nm(0x3008, 1, 2)});
    rig.run(1);
    CHECK(rig.r(1) == 7u);

    Sh2Rig b;
    b.r(1) = 0; b.r(2) = 1;
    b.load({nm(0x300A, 1, 2)});
    b.run(1);
    CHECK(b.r(1) == 0xFFFFFFFFu);
    CHECK(t(b));

    Sh2Rig c;
    c.r(1) = 0x80000000; c.r(2) = 1;
    c.load({nm(0x300B, 1, 2)});
    c.run(1);
    CHECK(c.r(1) == 0x7FFFFFFFu);
    CHECK(t(c));
}

TEST_CASE("neg e negc") {
    Sh2Rig rig;
    rig.r(2) = 5;
    rig.load({nm(0x600B, 1, 2)});
    rig.run(1);
    CHECK(rig.r(1) == 0xFFFFFFFBu);

    struct Case { uint32_t m; bool t_in; uint32_t result; bool t_out; };
    for (Case k : {Case{0, false, 0, false}, Case{1, false, 0xFFFFFFFF, true}, Case{0, true, 0xFFFFFFFF, true}}) {
        Sh2Rig b;
        b.r(2) = k.m;
        if (k.t_in) b.s().sr |= 1;
        b.load({nm(0x600A, 1, 2)});
        b.run(1);
        CHECK(b.r(1) == k.result);
        CHECK(t(b) == k.t_out);
    }
}

TEST_CASE("cmp/eq, hs, ge, hi, gt: sem sinal e com sinal") {
    struct Case { uint16_t op; uint32_t rn, rm; bool expect; };
    const Case cases[] = {
        {nm(0x3000, 1, 2), 5, 5, true},          {nm(0x3000, 1, 2), 5, 6, false},          // eq
        {nm(0x3002, 1, 2), 0xFFFFFFFF, 1, true},  {nm(0x3002, 1, 2), 1, 2, false},          // hs: Rn >= Rm (sem sinal)
        {nm(0x3003, 1, 2), 0xFFFFFFFF, 1, false}, {nm(0x3003, 1, 2), 2, 2, true},           // ge: com sinal
        {nm(0x3006, 1, 2), 5, 5, false},          {nm(0x3006, 1, 2), 6, 5, true},           // hi: Rn > Rm (sem sinal)
        {nm(0x3007, 1, 2), 0xFFFFFFFF, 0xFFFFFFFE, true}, {nm(0x3007, 1, 2), 0xFFFFFFFE, 0xFFFFFFFF, false},  // gt
    };
    for (const auto& k : cases) {
        Sh2Rig rig;
        rig.r(1) = k.rn; rig.r(2) = k.rm;
        rig.load({k.op});
        rig.run(1);
        CHECK(t(rig) == k.expect);
    }
}

TEST_CASE("cmp/pz, cmp/pl, cmp/str e cmp/eq #imm,R0") {
    struct Case { uint16_t op; uint32_t rn, rm; bool expect; };
    const Case cases[] = {
        {n_(0x4011, 1), 0, 0, true},  {n_(0x4011, 1), 0xFFFFFFFF, 0, false},   // pz: Rn >= 0
        {n_(0x4015, 1), 0, 0, false}, {n_(0x4015, 1), 1, 0, true},             // pl: Rn > 0
        {nm(0x200C, 1, 2), 0x11223344, 0xAA33BBCC, false},                      // str: bytes correspondentes
        {nm(0x200C, 1, 2), 0x11223344, 0xAA22BBCC, true},
    };
    for (const auto& k : cases) {
        Sh2Rig rig;
        rig.r(1) = k.rn; rig.r(2) = k.rm;
        rig.load({k.op});
        rig.run(1);
        CHECK(t(rig) == k.expect);
    }
    Sh2Rig b;
    b.r(0) = 0xFFFFFFFF;
    b.load({0x8800 | 0xFF});  // cmp/eq #-1,r0
    b.run(1);
    CHECK(t(b));
}

TEST_CASE("dt decrementa e testa zero") {
    Sh2Rig rig;
    rig.r(1) = 1; rig.r(2) = 0;
    rig.load({n_(0x4010, 1), n_(0x4010, 2)});
    rig.run(1);
    CHECK(rig.r(1) == 0u);
    CHECK(t(rig));
    rig.run(1);
    CHECK(rig.r(2) == 0xFFFFFFFFu);
    CHECK_FALSE(t(rig));
}

TEST_CASE("exts.b/w e extu.b/w") {
    Sh2Rig rig;
    rig.r(2) = 0x12348081;
    rig.load({nm(0x600E, 1, 2), nm(0x600F, 3, 2), nm(0x600C, 4, 2), nm(0x600D, 5, 2)});
    rig.run(4);
    CHECK(rig.r(1) == 0xFFFFFF81u);
    CHECK(rig.r(3) == 0xFFFF8081u);
    CHECK(rig.r(4) == 0x81u);
    CHECK(rig.r(5) == 0x8081u);
}

TEST_CASE("and, or, xor, not e tst") {
    Sh2Rig rig;
    rig.r(1) = 0xF0F0F0F0; rig.r(2) = 0xFF00FF00;
    rig.load({nm(0x2009, 1, 2), nm(0x200B, 3, 2), nm(0x200A, 4, 2), nm(0x6007, 5, 2)});
    rig.r(3) = 0x0000000F; rig.r(4) = 0xFFFFFFFF;
    rig.run(4);
    CHECK(rig.r(1) == 0xF000F000u);
    CHECK(rig.r(3) == 0xFF00FF0Fu);
    CHECK(rig.r(4) == 0x00FF00FFu);
    CHECK(rig.r(5) == 0x00FF00FFu);

    Sh2Rig b;
    b.r(1) = 0x0F; b.r(2) = 0xF0;
    b.load({nm(0x2008, 1, 2)});  // tst r2,r1 -> T = (r1 & r2) == 0
    b.run(1);
    CHECK(t(b));
}

TEST_CASE("operações com imediato em R0 (zero-extend): and, or, xor, tst") {
    Sh2Rig rig;
    rig.r(0) = 0xFFFFFFFF;
    rig.load({0xC900 | 0x0F, 0xCB00 | 0xF0, 0xCA00 | 0xFF, 0xC800 | 0x01});  // and #0x0F ; or #0xF0 ; xor #0xFF ; tst #1
    rig.run(1);
    CHECK(rig.r(0) == 0x0Fu);               // zero-extend: não preserva bits altos
    rig.run(1);
    CHECK(rig.r(0) == 0xFFu);
    rig.run(1);
    CHECK(rig.r(0) == 0u);
    rig.run(1);
    CHECK(t(rig));                          // 0 & 1 == 0
}

TEST_CASE("operações em byte @(R0,GBR): and.b, or.b, xor.b, tst.b e tas.b") {
    Sh2Rig rig;
    rig.s().gbr = 0x06000100; rig.r(0) = 4;
    rig.bus.write8(0x06000104, 0xF3);
    rig.load({0xCD00 | 0x0F, 0xCF00 | 0x80, 0xCE00 | 0x01, 0xCC00 | 0x80});
    rig.run(1);  CHECK(rig.bus.read8(0x06000104) == 0x03);   // and.b
    rig.run(1);  CHECK(rig.bus.read8(0x06000104) == 0x83);   // or.b
    rig.run(1);  CHECK(rig.bus.read8(0x06000104) == 0x82);   // xor.b
    rig.run(1);  CHECK_FALSE(t(rig));                         // tst.b: 0x82 & 0x80 != 0

    Sh2Rig b;
    b.r(1) = 0x06000200;
    b.bus.write8(0x06000200, 0x00);
    b.load({n_(0x401B, 1), n_(0x401B, 1)});  // tas.b @r1 ×2
    b.run(1);
    CHECK(t(b));                              // era 0
    CHECK(b.bus.read8(0x06000200) == 0x80);
    b.run(1);
    CHECK_FALSE(t(b));                        // já tinha o bit 7
}

TEST_CASE("deslocamentos e rotações de 1 bit atualizam T") {
    struct Case { uint16_t op; uint32_t rn; bool t_in; uint32_t result; bool t_out; };
    const Case cases[] = {
        {n_(0x4000, 1), 0x80000001, false, 0x00000002, true},   // shll
        {n_(0x4020, 1), 0x80000001, false, 0x00000002, true},   // shal (igual ao shll)
        {n_(0x4001, 1), 0x80000001, false, 0x40000000, true},   // shlr
        {n_(0x4021, 1), 0x80000001, false, 0xC0000000, true},   // shar: preserva o sinal
        {n_(0x4004, 1), 0x80000001, false, 0x00000003, true},   // rotl
        {n_(0x4005, 1), 0x80000001, false, 0xC0000000, true},   // rotr
        {n_(0x4024, 1), 0x80000001, true,  0x00000003, true},   // rotcl: entra T=1 pela direita
        {n_(0x4025, 1), 0x80000001, true,  0xC0000000, true},   // rotcr: entra T=1 pela esquerda
        {n_(0x4025, 1), 0x00000002, false, 0x00000001, false},  // rotcr sem carry
    };
    for (const auto& k : cases) {
        Sh2Rig rig;
        rig.r(1) = k.rn;
        if (k.t_in) rig.s().sr |= 1;
        rig.load({k.op});
        rig.run(1);
        CHECK(rig.r(1) == k.result);
        CHECK(t(rig) == k.t_out);
    }
}

TEST_CASE("deslocamentos fixos de 2, 8 e 16 bits não mexem em T") {
    Sh2Rig rig;
    rig.r(1) = 0x12345678;
    rig.s().sr |= 1;
    rig.load({n_(0x4008, 1), n_(0x4009, 1), n_(0x4018, 1), n_(0x4019, 1), n_(0x4028, 1), n_(0x4029, 1)});
    rig.run(1);  CHECK(rig.r(1) == 0x48D159E0u);  // shll2
    rig.run(1);  CHECK(rig.r(1) == 0x12345678u);  // shlr2
    rig.run(1);  CHECK(rig.r(1) == 0x34567800u);  // shll8
    rig.run(1);  CHECK(rig.r(1) == 0x00345678u);  // shlr8
    rig.run(1);  CHECK(rig.r(1) == 0x56780000u);  // shll16
    rig.run(1);  CHECK(rig.r(1) == 0x00005678u);  // shlr16
    CHECK(t(rig));
}

TEST_CASE("clrt, sett e clrmac") {
    Sh2Rig rig;
    rig.s().mach = 5; rig.s().macl = 6;
    rig.load({0x0018, 0x0008, 0x0028});  // sett ; clrt ; clrmac
    rig.run(1);  CHECK(t(rig));
    rig.run(1);  CHECK_FALSE(t(rig));
    rig.run(1);
    CHECK(rig.s().mach == 0u);
    CHECK(rig.s().macl == 0u);
}
