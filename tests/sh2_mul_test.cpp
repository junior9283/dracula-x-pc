#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "sh2_harness.hpp"

using testsupport::Sh2Rig;

namespace {
constexpr uint16_t nm(uint16_t base, int n, int m) { return static_cast<uint16_t>(base | (n << 8) | (m << 4)); }
}  // namespace

TEST_CASE("mul.l, muls.w e mulu.w gravam em MACL") {
    Sh2Rig rig;
    rig.r(1) = 0x10000; rig.r(2) = 0x10001;
    rig.load({nm(0x0007, 1, 2)});
    rig.run(1);
    CHECK(rig.s().macl == 0x00010000u);   // só os 32 bits baixos

    Sh2Rig b;
    b.r(1) = 0x0000FFFF; b.r(2) = 3;
    b.load({nm(0x200F, 1, 2)});          // muls.w: -1 × 3
    b.run(1);
    CHECK(b.s().macl == 0xFFFFFFFDu);

    Sh2Rig c;
    c.r(1) = 0x0000FFFF; c.r(2) = 3;
    c.load({nm(0x200E, 1, 2)});          // mulu.w: 65535 × 3
    c.run(1);
    CHECK(c.s().macl == 0x0002FFFDu);
}

TEST_CASE("dmuls.l e dmulu.l: produto de 64 bits em MACH:MACL") {
    Sh2Rig rig;
    rig.r(1) = 0xFFFFFFFF; rig.r(2) = 0x7FFFFFFF;
    rig.load({nm(0x300D, 1, 2)});
    rig.run(1);
    CHECK(rig.s().mach == 0xFFFFFFFFu);
    CHECK(rig.s().macl == 0x80000001u);

    Sh2Rig b;
    b.r(1) = 0xFFFFFFFF; b.r(2) = 0xFFFFFFFF;
    b.load({nm(0x3005, 1, 2)});
    b.run(1);
    CHECK(b.s().mach == 0xFFFFFFFEu);
    CHECK(b.s().macl == 1u);
}

TEST_CASE("mac.l sem saturação acumula em 64 bits; com S limita a 48 bits") {
    Sh2Rig rig;
    rig.s().macl = 5;
    rig.bus.write32(0x06000100, 0x10000); rig.bus.write32(0x06000200, 0x10000);
    rig.r(1) = 0x06000100; rig.r(2) = 0x06000200;
    rig.load({nm(0x000F, 1, 2)});        // mac.l @r2+,@r1+
    rig.run(1);
    CHECK(rig.s().mach == 1u);
    CHECK(rig.s().macl == 5u);           // 5 + 2^32
    CHECK(rig.r(1) == 0x06000104u);
    CHECK(rig.r(2) == 0x06000204u);

    Sh2Rig b;
    b.s().sr |= 2;                       // S
    b.s().mach = 0x00007FFF; b.s().macl = 0xFFFFFFFF;
    b.bus.write32(0x06000100, 1); b.bus.write32(0x06000200, 1);
    b.r(1) = 0x06000100; b.r(2) = 0x06000200;
    b.load({nm(0x000F, 1, 2)});
    b.run(1);
    CHECK(b.s().mach == 0x00007FFFu);    // continua no máximo positivo de 48 bits
    CHECK(b.s().macl == 0xFFFFFFFFu);
}

TEST_CASE("mac.w sem S acumula em 64 bits; com S satura em 32 bits e marca MACH") {
    Sh2Rig rig;
    rig.s().macl = 100;
    rig.bus.write16(0x06000100, 0xFFFE); rig.bus.write16(0x06000200, 3);   // (-2) × 3
    rig.r(1) = 0x06000100; rig.r(2) = 0x06000200;
    rig.load({nm(0x400F, 1, 2)});        // mac.w @r2+,@r1+
    rig.run(1);
    CHECK(rig.s().macl == 94u);
    CHECK(rig.r(1) == 0x06000102u);

    Sh2Rig b;
    b.s().sr |= 2;
    b.s().macl = 0x7FFFFFF0;
    b.bus.write16(0x06000100, 0x7FFF); b.bus.write16(0x06000200, 0x7FFF);  // produto 0x3FFF0001
    b.r(1) = 0x06000100; b.r(2) = 0x06000200;
    b.load({nm(0x400F, 1, 2)});
    b.run(1);
    CHECK(b.s().macl == 0x7FFFFFFFu);
    CHECK((b.s().mach & 1) == 1u);

    Sh2Rig c;
    c.s().sr |= 2;
    c.s().macl = 0x80000010;
    c.bus.write16(0x06000100, 0x8000); c.bus.write16(0x06000200, 0x7FFF);  // produto negativo grande
    c.r(1) = 0x06000100; c.r(2) = 0x06000200;
    c.load({nm(0x400F, 1, 2)});
    c.run(1);
    CHECK(c.s().macl == 0x80000000u);
    CHECK((c.s().mach & 1) == 1u);
}

TEST_CASE("div0s e div0u preparam Q, M e T") {
    Sh2Rig rig;
    rig.r(1) = 0x80000000; rig.r(2) = 1;
    rig.load({nm(0x2007, 1, 2)});        // div0s r2,r1: Q = bit 31 de Rn, M = bit 31 de Rm, T = Q ^ M
    rig.run(1);
    CHECK((rig.s().sr & sh2::kSrQ) != 0);
    CHECK((rig.s().sr & sh2::kSrM) == 0);
    CHECK((rig.s().sr & sh2::kSrT) != 0);

    rig.load({0x0019});                  // div0u
    rig.s().pc = Sh2Rig::kCode;
    rig.run(1);
    CHECK((rig.s().sr & (sh2::kSrQ | sh2::kSrM | sh2::kSrT)) == 0);
}

namespace {
// Divisão sem sinal 32/16 do manual: r0 = dividendo, r1 = divisor << 16; resultado (quociente de 16 bits) em r0.
uint32_t divide_32_by_16(uint32_t dividend, uint32_t divisor) {
    Sh2Rig rig;
    std::vector<uint16_t> program = {0x0019};                                  // div0u
    for (int i = 0; i < 16; ++i) program.push_back(nm(0x3004, 0, 1));          // div1 r1,r0
    program.push_back(0x4024);                                                  // rotcl r0
    program.push_back(nm(0x600D, 0, 0));                                        // extu.w r0,r0
    uint32_t address = Sh2Rig::kCode;
    for (uint16_t w : program) { rig.bus.write16(address, w); address += 2; }
    rig.r(0) = dividend;
    rig.r(1) = divisor << 16;
    rig.run(static_cast<int>(program.size()));
    return rig.r(0);
}

// Divisão sem sinal 32/32 do manual: r0 = dividendo, r1 = divisor, r3 = 0; quociente em r0.
// (O resto em r3 é parcial — a divisão é não restauradora — e a sequência do manual só garante o quociente.)
uint32_t divide_32_by_32(uint32_t dividend, uint32_t divisor) {
    Sh2Rig rig;
    std::vector<uint16_t> program = {0x0019};                                  // div0u
    for (int i = 0; i < 32; ++i) {
        program.push_back(0x4024);                                              // rotcl r0
        program.push_back(nm(0x3004, 3, 1));                                    // div1 r1,r3
    }
    program.push_back(0x4024);                                                  // rotcl r0
    uint32_t address = Sh2Rig::kCode;
    for (uint16_t w : program) { rig.bus.write16(address, w); address += 2; }
    rig.r(0) = dividend; rig.r(1) = divisor; rig.r(3) = 0;
    rig.run(static_cast<int>(program.size()));
    return rig.r(0);
}
}  // namespace

TEST_CASE("div1: sequência 32/16 sem sinal bate com a divisão inteira de referência") {
    struct Case { uint32_t dividend, divisor; };
    for (Case k : {Case{100000, 7}, Case{0, 5}, Case{12345, 1}, Case{0x7FFFFFFF, 0x8000}, Case{1234567890, 40000}, Case{65535, 65535}}) {
        CHECK(divide_32_by_16(k.dividend, k.divisor) == k.dividend / k.divisor);
    }
}

TEST_CASE("div1: sequência 32/32 sem sinal bate com o quociente de referência") {
    struct Case { uint32_t dividend, divisor; };
    for (Case k : {Case{100000, 7}, Case{0xFFFFFFFF, 3}, Case{0xFFFFFFFF, 0xFFFFFFFF}, Case{1, 0xFFFFFFFF}, Case{0x80000000, 0x7FFFFFFF}, Case{123456789, 1000}}) {
        CHECK(divide_32_by_32(k.dividend, k.divisor) == k.dividend / k.divisor);
    }
}
