// Testes do interpretador do Motorola 68000 (src/audio/m68k.*).
// Programas montados a mao em hexadecimal, carregados em 0x400; pilha de supervisor em 0x2000.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "audio/m68k.hpp"

using audio::M68k;

namespace {

constexpr uint16_t C = M68k::kC, V = M68k::kV, Z = M68k::kZ, N = M68k::kN, X = M68k::kX;

// RAM de teste (1 MiB, espelhada nos 24 bits) com registro opcional das escritas.
struct Ram : M68k::Bus {
    struct Write { uint32_t addr; int size; uint32_t value; };
    std::vector<uint8_t> m = std::vector<uint8_t>(1 << 20, 0);
    std::vector<Write> log;

    uint8_t read8(uint32_t a) override { return m[a & 0xFFFFF]; }
    uint16_t read16(uint32_t a) override { return static_cast<uint16_t>((m[a & 0xFFFFF] << 8) | m[(a + 1) & 0xFFFFF]); }
    void write8(uint32_t a, uint8_t v) override { m[a & 0xFFFFF] = v; log.push_back({a, 1, v}); }
    void write16(uint32_t a, uint16_t v) override {
        m[a & 0xFFFFF] = static_cast<uint8_t>(v >> 8);
        m[(a + 1) & 0xFFFFF] = static_cast<uint8_t>(v);
        log.push_back({a, 2, v});
    }
    void w16(uint32_t a, uint16_t v) { m[a] = static_cast<uint8_t>(v >> 8); m[a + 1] = static_cast<uint8_t>(v); }
    void w32(uint32_t a, uint32_t v) { w16(a, static_cast<uint16_t>(v >> 16)); w16(a + 2, static_cast<uint16_t>(v)); }
    uint16_t r16(uint32_t a) const { return static_cast<uint16_t>((m[a] << 8) | m[a + 1]); }
    uint32_t r32(uint32_t a) const { return (uint32_t{r16(a)} << 16) | r16(a + 2); }
};

struct Rig {
    Ram ram;
    M68k cpu{ram};

    Rig() {
        ram.w32(0, 0x2000);
        ram.w32(4, 0x400);
        cpu.reset();
        cpu.set_sr(0x2000);  // supervisor, mascara 0, flags limpas
    }
    void code(std::initializer_list<uint16_t> words, uint32_t at = 0x400) {
        for (uint16_t w : words) { ram.w16(at, w); at += 2; }
    }
    void run(int n) { for (int i = 0; i < n; ++i) cpu.step(); }
    uint16_t ccr() const { return cpu.sr() & 0x1F; }
    void set_ccr(uint16_t f) { cpu.set_sr(static_cast<uint16_t>((cpu.sr() & 0xFF00) | f)); }
    void vector(int n, uint32_t handler) { ram.w32(static_cast<uint32_t>(n) * 4, handler); }
    void fill(uint32_t at, std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) ram.m[at++] = b; }
};

// Monta opcode de deslocamento/rotacao em registrador. type: 0 AS, 1 LS, 2 ROX, 3 RO. size: 0 B, 1 W, 2 L.
constexpr uint16_t shreg(int type, bool left, int size, int cnt, bool by_reg, int reg) {
    return static_cast<uint16_t>(0xE000 | ((cnt & 7) << 9) | (left ? 0x100 : 0) | (size << 6) |
                                 (by_reg ? 0x20 : 0) | (type << 3) | reg);
}
// Deslocamento de memoria (palavra, 1 posicao) em (A0).
constexpr uint16_t shmem(int type, bool left) {
    return static_cast<uint16_t>(0xE0D0 | (type << 9) | (left ? 0x100 : 0));
}

}  // namespace

// ------------------------------------------------------------------ reset / MOVE

TEST_CASE("reset le SSP e PC dos vetores 0 e 4", "[m68k]") {
    Ram ram;
    ram.w32(0, 0x1234);
    ram.w32(4, 0x5678);
    M68k cpu(ram);
    cpu.reset();
    CHECK(cpu.a[7] == 0x1234u);
    CHECK(cpu.ssp() == 0x1234u);
    CHECK(cpu.pc == 0x5678u);
    CHECK(cpu.sr() == 0x2700);
}

TEST_CASE("MOVE entre registradores nos tres tamanhos", "[m68k]") {
    Rig r;
    r.code({0x1001, 0x3401, 0x2601});  // MOVE.B D1,D0 ; MOVE.W D1,D2 ; MOVE.L D1,D3
    r.cpu.d[1] = 0x12345678; r.cpu.d[0] = 0xAABBCCDD; r.cpu.d[2] = 0x11111111;
    r.run(3);
    CHECK(r.cpu.d[0] == 0xAABBCC78u);
    CHECK(r.cpu.d[2] == 0x11115678u);
    CHECK(r.cpu.d[3] == 0x12345678u);
}

TEST_CASE("MOVE imediato ajusta N/Z, zera V/C e preserva X", "[m68k]") {
    Rig r;
    r.code({0x203C, 0x8000, 0x0000, 0x323C, 0x0000});  // MOVE.L #$80000000,D0 ; MOVE.W #0,D1
    r.set_ccr(V | C | X);
    r.run(1);
    CHECK(r.cpu.d[0] == 0x80000000u);
    CHECK(r.ccr() == (N | X));
    r.run(1);
    CHECK(r.ccr() == (Z | X));
}

TEST_CASE("MOVE com (An)+, -(An), d16(An), abs e indice", "[m68k]") {
    Rig r;
    r.fill(0x1000, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
    r.cpu.a[0] = 0x1000;
    r.code({0x3218});  // MOVE.W (A0)+,D1
    r.run(1);
    CHECK((r.cpu.d[1] & 0xFFFF) == 0x1122u);
    CHECK(r.cpu.a[0] == 0x1002u);

    Rig b;
    b.fill(0x1000, {0x11, 0x22, 0x33, 0x44});
    b.cpu.a[0] = 0x1004;
    b.code({0x1420});  // MOVE.B -(A0),D2
    b.run(1);
    CHECK((b.cpu.d[2] & 0xFF) == 0x44u);
    CHECK(b.cpu.a[0] == 0x1003u);

    Rig c;
    c.fill(0x1000, {0, 0, 0, 0, 0x55, 0x66, 0x77});
    c.cpu.a[0] = 0x1000;
    c.code({0x3628, 0x0004});  // MOVE.W 4(A0),D3
    c.run(1);
    CHECK((c.cpu.d[3] & 0xFFFF) == 0x5566u);

    Rig d;
    d.fill(0x1000, {0, 0, 0, 0, 0, 0x66});
    d.cpu.a[0] = 0x1000;
    d.cpu.d[1] = 0xFFFF0003;  // indice .W ignora a metade alta
    d.code({0x1030, 0x1002});  // MOVE.B 2(A0,D1.W),D0
    d.run(1);
    CHECK((d.cpu.d[0] & 0xFF) == 0x66u);
}

TEST_CASE("MOVE com absoluto curto/longo", "[m68k]") {
    Rig r;
    r.ram.w16(0x1000, 0x1122);
    r.code({0x3039, 0x0000, 0x1000, 0x31C0, 0x1010});  // MOVE.W $1000,D0 ; MOVE.W D0,$1010.W
    r.run(2);
    CHECK((r.cpu.d[0] & 0xFFFF) == 0x1122u);
    CHECK(r.ram.r16(0x1010) == 0x1122);
}

TEST_CASE("MOVE.B com (A7)+ avanca 2 e longo usa duas escritas de 16 bits", "[m68k]") {
    Rig r;
    r.ram.m[0x2000] = 0x9A;
    r.code({0x101F});  // MOVE.B (A7)+,D0
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x9Au);
    CHECK(r.cpu.a[7] == 0x2002u);

    Rig w;
    w.cpu.a[0] = 0xFF001000;  // endereco mascarado em 24 bits
    w.cpu.d[0] = 0xDEADBEEF;
    w.code({0x2080});  // MOVE.L D0,(A0)
    w.ram.log.clear();
    w.run(1);
    REQUIRE(w.ram.log.size() == 2);
    CHECK(w.ram.log[0].addr == 0x1000u);
    CHECK(w.ram.log[0].size == 2);
    CHECK(w.ram.log[0].value == 0xDEADu);
    CHECK(w.ram.log[1].addr == 0x1002u);
    CHECK(w.ram.log[1].value == 0xBEEFu);
}

TEST_CASE("MOVEA estende o sinal e nao mexe nas flags; MOVEQ", "[m68k]") {
    Rig r;
    r.code({0x3240, 0x70FF});  // MOVEA.W D0,A1 ; MOVEQ #-1,D0
    r.cpu.d[0] = 0x8000;
    r.set_ccr(Z);
    r.run(1);
    CHECK(r.cpu.a[1] == 0xFFFF8000u);
    CHECK(r.ccr() == Z);
    r.run(1);
    CHECK(r.cpu.d[0] == 0xFFFFFFFFu);
    CHECK(r.ccr() == N);
}

// ------------------------------------------------------------------ aritmetica

TEST_CASE("ADD com flags de carry, overflow e extend", "[m68k]") {
    Rig r;
    r.code({0xD001});  // ADD.B D1,D0
    r.cpu.d[0] = 0x7F; r.cpu.d[1] = 1;
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x80u);
    CHECK(r.ccr() == (N | V));

    Rig b;
    b.code({0xD001});
    b.cpu.d[0] = 0xFF; b.cpu.d[1] = 1;
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0u);
    CHECK(b.ccr() == (Z | C | X));

    Rig c;
    c.code({0xD081});  // ADD.L D1,D0
    c.cpu.d[0] = 0x80000000; c.cpu.d[1] = 0x80000000;
    c.run(1);
    CHECK(c.cpu.d[0] == 0u);
    CHECK(c.ccr() == (Z | V | C | X));

    Rig e;
    e.code({0xD150});  // ADD.W D0,(A0)
    e.cpu.a[0] = 0x1000; e.cpu.d[0] = 2; e.ram.w16(0x1000, 1);
    e.run(1);
    CHECK(e.ram.r16(0x1000) == 3);
}

TEST_CASE("SUB com flags", "[m68k]") {
    Rig r;
    r.code({0x9001});  // SUB.B D1,D0
    r.cpu.d[0] = 0x80; r.cpu.d[1] = 1;
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x7Fu);
    CHECK(r.ccr() == V);

    Rig b;
    b.code({0x9001});
    b.cpu.d[0] = 0; b.cpu.d[1] = 1;
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0xFFu);
    CHECK(b.ccr() == (N | C | X));

    Rig c;
    c.code({0x9001});
    c.cpu.d[0] = 7; c.cpu.d[1] = 7;
    c.run(1);
    CHECK(c.ccr() == Z);
}

TEST_CASE("ADDQ/SUBQ/ADDI/SUBI/CMPI e ADDA/SUBA", "[m68k]") {
    Rig r;
    r.code({0x5640, 0x5040, 0x5388, 0x5248});  // ADDQ.W #3,D0 ; ADDQ.W #8,D0 ; SUBQ.L #1,A0 ; ADDQ.W #1,A0
    r.cpu.d[0] = 0xFFFF0000; r.cpu.a[0] = 0x1000;
    r.run(1);
    CHECK(r.cpu.d[0] == 0xFFFF0003u);
    r.run(1);
    CHECK(r.cpu.d[0] == 0xFFFF000Bu);
    r.set_ccr(Z | C);
    r.run(2);
    CHECK(r.cpu.a[0] == 0x1000u);  // -1 +1, sem tocar nas flags
    CHECK(r.ccr() == (Z | C));

    Rig b;
    b.code({0x0600, 0x0005, 0x0440, 0x0001, 0x0C80, 0x0000, 0x0005});  // ADDI.B #5,D0 ; SUBI.W #1,D0 ; CMPI.L #5,D0
    b.cpu.d[0] = 0x000000FE;
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0x03u);
    CHECK(b.ccr() == (C | X));
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFFFF) == 0x0002u);
    b.cpu.d[0] = 5;
    b.run(1);
    CHECK(b.ccr() == Z);
    CHECK(b.cpu.d[0] == 5u);

    Rig c;
    c.code({0xD2C0, 0xD3C0, 0x93C0});  // ADDA.W D0,A1 ; ADDA.L D0,A1 ; SUBA.L D0,A1
    c.cpu.d[0] = 0x8000; c.cpu.a[1] = 0x100000;
    c.set_ccr(N);
    c.run(1);
    CHECK(c.cpu.a[1] == 0x100000u - 0x8000u);  // palavra com sinal
    c.run(1);
    CHECK(c.cpu.a[1] == 0x100000u - 0x8000u + 0x8000u);
    c.run(1);
    CHECK(c.cpu.a[1] == 0x100000u - 0x8000u);
    CHECK(c.ccr() == N);
}

TEST_CASE("NEG/NEGX/NOT/CLR/TST", "[m68k]") {
    Rig r;
    r.code({0x4440});  // NEG.W D0
    r.cpu.d[0] = 0x8000;
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFFFF) == 0x8000u);
    CHECK(r.ccr() == (N | V | C | X));

    Rig b;
    b.code({0x4440});
    b.run(1);
    CHECK(b.ccr() == Z);

    Rig c;
    c.code({0x4040});  // NEGX.W D0
    c.set_ccr(X);
    c.run(1);
    CHECK((c.cpu.d[0] & 0xFFFF) == 0xFFFFu);
    CHECK(c.ccr() == (N | C | X));

    Rig d;
    d.code({0x4600, 0x4280, 0x4A40});  // NOT.B D0 ; CLR.L D0 ; TST.W D0
    d.cpu.d[0] = 0x12345678;
    d.run(1);
    CHECK(d.cpu.d[0] == 0x12345687u);
    d.run(1);
    CHECK(d.cpu.d[0] == 0u);
    CHECK(d.ccr() == Z);
    d.cpu.d[0] = 0x8000;
    d.run(1);
    CHECK(d.ccr() == N);
}

TEST_CASE("ADDX/SUBX multiprecisao", "[m68k]") {
    Rig r;  // D0:D1 + D2:D3
    r.code({0xD283, 0xD182});  // ADD.L D3,D1 ; ADDX.L D2,D0
    r.cpu.d[0] = 1; r.cpu.d[1] = 0xFFFFFFFF; r.cpu.d[2] = 0; r.cpu.d[3] = 1;
    r.run(2);
    CHECK(r.cpu.d[1] == 0u);
    CHECK(r.cpu.d[0] == 2u);
    CHECK(!(r.cpu.sr() & Z));

    Rig b;
    b.code({0x9283, 0x9182});  // SUB.L D3,D1 ; SUBX.L D2,D0
    b.cpu.d[0] = 2; b.cpu.d[1] = 0; b.cpu.d[2] = 0; b.cpu.d[3] = 1;
    b.run(2);
    CHECK(b.cpu.d[1] == 0xFFFFFFFFu);
    CHECK(b.cpu.d[0] == 1u);

    Rig c;  // resultado zero nao seta Z (so limpa)
    c.code({0xD101});  // ADDX.B D1,D0
    c.set_ccr(0);
    c.run(1);
    CHECK(c.ccr() == 0);

    Rig d;  // forma de memoria ADDX.W -(A1),-(A0)
    d.code({0xD149});
    d.cpu.a[0] = 0x1002; d.cpu.a[1] = 0x1012;
    d.ram.w16(0x1000, 0x00FF); d.ram.w16(0x1010, 0x0001);
    d.set_ccr(X);
    d.run(1);
    CHECK(d.ram.r16(0x1000) == 0x0101);
    CHECK(d.cpu.a[0] == 0x1000u);
    CHECK(d.cpu.a[1] == 0x1010u);
}

// ------------------------------------------------------------------ CMP / Bcc

TEST_CASE("CMP + Scc cobre as 16 condicoes", "[m68k]") {
    const int32_t pares[][2] = {
        {0, 0}, {1, 2}, {2, 1}, {0x7FFFFFFF, -1}, {INT32_MIN, 1}, {-1, 1}, {-1, -1}, {5, INT32_MIN},
    };
    for (const auto& p : pares) {
        const uint32_t u0 = static_cast<uint32_t>(p[0]), u1 = static_cast<uint32_t>(p[1]);
        const int32_t s0 = p[0], s1 = p[1];
        int32_t dummy;
        const bool ov = __builtin_sub_overflow(s0, s1, &dummy);
        const bool neg = static_cast<int32_t>(u0 - u1) < 0;
        const bool esperado[16] = {
            true, false, u0 > u1, u0 <= u1, u0 >= u1, u0 < u1, u0 != u1, u0 == u1,
            !ov, ov, !neg, neg, s0 >= s1, s0 < s1, s0 > s1, s0 <= s1,
        };
        for (int cc = 0; cc < 16; ++cc) {
            Rig r;
            r.code({0xB081, static_cast<uint16_t>(0x50C2 | (cc << 8))});  // CMP.L D1,D0 ; Scc D2
            r.cpu.d[0] = u0; r.cpu.d[1] = u1; r.cpu.d[2] = 0x12345600;
            r.run(2);
            INFO("D0=" << s0 << " D1=" << s1 << " cc=" << cc);
            CHECK((r.cpu.d[2] & 0xFF) == (esperado[cc] ? 0xFFu : 0x00u));
            CHECK((r.cpu.d[2] & ~0xFFu) == 0x12345600u);
        }
    }
}

TEST_CASE("Bcc tomado e nao tomado, BRA.W", "[m68k]") {
    // CMP.L D1,D0 ; BEQ.B +4 ; MOVEQ #1,D2 ; BRA.B +2 ; MOVEQ #2,D2 ; NOP
    const std::initializer_list<uint16_t> prog = {0xB081, 0x6704, 0x7401, 0x6002, 0x7402, 0x4E71};
    Rig eq;
    eq.code(prog);
    eq.cpu.d[0] = eq.cpu.d[1] = 9;
    eq.run(3);
    CHECK(eq.cpu.d[2] == 2u);
    CHECK(eq.cpu.pc == 0x40Au);

    Rig ne;
    ne.code(prog);
    ne.cpu.d[0] = 9; ne.cpu.d[1] = 8;
    ne.run(4);
    CHECK(ne.cpu.d[2] == 1u);
    CHECK(ne.cpu.pc == 0x40Au);

    Rig w;
    w.code({0x6000, 0x0004, 0x7401, 0x7402});  // BRA.W +4 pula o MOVEQ #1
    w.run(2);
    CHECK(w.cpu.d[2] == 2u);
    CHECK(w.cpu.pc == 0x408u);
}

TEST_CASE("CMPA e CMPM", "[m68k]") {
    Rig r;
    r.code({0xB2C0});  // CMPA.W D0,A1
    r.cpu.d[0] = 0xFFFF; r.cpu.a[1] = 0xFFFFFFFF;  // -1 == -1
    r.run(1);
    CHECK(r.ccr() == Z);

    Rig b;
    b.code({0xB109, 0xB109});  // CMPM.B (A1)+,(A0)+ x2
    b.cpu.a[0] = 0x1000; b.cpu.a[1] = 0x1010;
    b.fill(0x1000, {5, 3});
    b.fill(0x1010, {5, 5});
    b.run(1);
    CHECK(b.ccr() == Z);
    b.run(1);
    CHECK(b.ccr() == (N | C));  // 3 - 5
    CHECK(b.cpu.a[0] == 0x1002u);
    CHECK(b.cpu.a[1] == 0x1012u);
}

TEST_CASE("DBcc em laco e com condicao verdadeira", "[m68k]") {
    Rig r;  // MOVEQ #4,D0 ; MOVEQ #0,D1 ; laco: ADDQ.W #1,D1 ; DBF D0,laco
    r.code({0x7004, 0x7200, 0x5241, 0x51C8, 0xFFFC});
    r.run(2 + 5 * 2);
    CHECK(r.cpu.d[1] == 5u);
    CHECK((r.cpu.d[0] & 0xFFFF) == 0xFFFFu);
    CHECK(r.cpu.pc == 0x40Au);

    Rig b;  // DBT nunca decrementa
    b.code({0x50C8, 0xFFFE});
    b.cpu.d[0] = 5;
    b.run(1);
    CHECK(b.cpu.d[0] == 5u);
    CHECK(b.cpu.pc == 0x404u);
}

// ------------------------------------------------------------------ MUL / DIV

TEST_CASE("MULU e MULS", "[m68k]") {
    Rig r;
    r.code({0xC0C1});  // MULU.W D1,D0
    r.cpu.d[0] = 0xABCDFFFF; r.cpu.d[1] = 0xFFFF;
    r.run(1);
    CHECK(r.cpu.d[0] == 0xFFFE0001u);
    CHECK(r.ccr() == N);

    Rig b;
    b.code({0xC1C1});  // MULS.W D1,D0
    b.cpu.d[0] = 0xFFFE; b.cpu.d[1] = 3;
    b.run(1);
    CHECK(b.cpu.d[0] == 0xFFFFFFFAu);
    CHECK(b.ccr() == N);
}

TEST_CASE("DIVU e DIVS com resto, estouro e divisao por zero", "[m68k]") {
    Rig r;
    r.code({0x80C1});  // DIVU.W D1,D0
    r.cpu.d[0] = 100; r.cpu.d[1] = 7;
    r.run(1);
    CHECK(r.cpu.d[0] == 0x0002000Eu);
    CHECK(r.ccr() == 0);

    Rig o;
    o.code({0x80C1});
    o.cpu.d[0] = 0x00100000; o.cpu.d[1] = 1;
    o.run(1);
    CHECK(o.cpu.d[0] == 0x00100000u);  // inalterado
    CHECK((o.ccr() & V) != 0);

    Rig s;
    s.code({0x81C1});  // DIVS.W D1,D0
    s.cpu.d[0] = static_cast<uint32_t>(-100); s.cpu.d[1] = 7;
    s.run(1);
    CHECK(s.cpu.d[0] == 0xFFFEFFF2u);  // resto -2, quociente -14
    CHECK(s.ccr() == N);

    Rig so;
    so.code({0x81C1});
    so.cpu.d[0] = 0x80000000; so.cpu.d[1] = 0xFFFF;  // -2^31 / -1 estoura
    so.run(1);
    CHECK((so.ccr() & V) != 0);
    CHECK(so.cpu.d[0] == 0x80000000u);

    Rig z;
    z.vector(5, 0x800);
    z.code({0x80C1});
    z.cpu.d[0] = 1; z.cpu.d[1] = 0;
    z.run(1);
    CHECK(z.cpu.pc == 0x800u);
    CHECK(z.cpu.a[7] == 0x1FFAu);
    CHECK(z.ram.r32(0x1FFC) == 0x402u);
}

// ------------------------------------------------------------------ shifts

TEST_CASE("shifts logicos e aritmeticos: flags X/C/V", "[m68k]") {
    Rig r;
    r.code({shreg(1, true, 0, 1, false, 0)});  // LSL.B #1,D0
    r.cpu.d[0] = 0x81;
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x02u);
    CHECK(r.ccr() == (C | X));

    Rig b;
    b.code({shreg(1, false, 0, 1, false, 0)});  // LSR.B #1,D0
    b.cpu.d[0] = 1;
    b.run(1);
    CHECK(b.ccr() == (Z | C | X));

    Rig c;
    c.code({shreg(0, false, 0, 1, false, 0)});  // ASR.B #1,D0
    c.cpu.d[0] = 0x81;
    c.run(1);
    CHECK((c.cpu.d[0] & 0xFF) == 0xC0u);
    CHECK(c.ccr() == (N | C | X));

    Rig d;
    d.code({shreg(0, true, 0, 1, false, 0)});  // ASL.B #1,D0
    d.cpu.d[0] = 0x40;
    d.run(1);
    CHECK((d.cpu.d[0] & 0xFF) == 0x80u);
    CHECK(d.ccr() == (N | V));

    Rig e;
    e.code({shreg(0, true, 1, 2, false, 0)});  // ASL.W #2,D0 : V acumulado
    e.cpu.d[0] = 0x4001;
    e.run(1);
    CHECK((e.cpu.d[0] & 0xFFFF) == 0x0004u);
    CHECK(e.ccr() == (V | C | X));

    Rig f;
    f.code({shreg(1, true, 0, 8, false, 0)});  // LSL.B #8,D0 (campo 0 = 8)
    f.cpu.d[0] = 1;
    f.run(1);
    CHECK((f.cpu.d[0] & 0xFF) == 0u);
    CHECK(f.ccr() == (Z | C | X));
}

TEST_CASE("rotates e rotates com extend", "[m68k]") {
    Rig r;
    r.code({shreg(3, true, 0, 1, false, 0)});  // ROL.B #1,D0
    r.cpu.d[0] = 0x81;
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x03u);
    CHECK(r.ccr() == C);  // X nao e afetado

    Rig b;
    b.code({shreg(3, false, 0, 1, false, 0)});  // ROR.B #1,D0
    b.cpu.d[0] = 1;
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0x80u);
    CHECK(b.ccr() == (N | C));

    Rig c;
    c.code({shreg(2, true, 1, 1, false, 0)});  // ROXL.W #1,D0
    c.cpu.d[0] = 0x8000;
    c.set_ccr(X);
    c.run(1);
    CHECK((c.cpu.d[0] & 0xFFFF) == 0x0001u);
    CHECK(c.ccr() == (C | X));

    Rig d;
    d.code({shreg(2, false, 0, 1, false, 0)});  // ROXR.B #1,D0
    d.cpu.d[0] = 1;
    d.run(1);
    CHECK((d.cpu.d[0] & 0xFF) == 0u);
    CHECK(d.ccr() == (Z | C | X));
}

TEST_CASE("shifts com contagem em registrador (0, 33)", "[m68k]") {
    Rig r;
    r.code({shreg(1, false, 2, 1, true, 0)});  // LSR.L D1,D0
    r.cpu.d[0] = 0xFFFFFFFF; r.cpu.d[1] = 33;
    r.run(1);
    CHECK(r.cpu.d[0] == 0u);
    CHECK(r.ccr() == Z);

    Rig b;  // contagem 0: N/Z do valor, C limpo, X preservado
    b.code({shreg(1, true, 0, 1, true, 0)});  // LSL.B D1,D0
    b.cpu.d[0] = 0x80; b.cpu.d[1] = 0;
    b.set_ccr(X | C);
    b.run(1);
    CHECK(b.ccr() == (N | X));

    Rig c;  // ROXL com contagem 0: C = X
    c.code({shreg(2, true, 0, 1, true, 0)});
    c.cpu.d[1] = 0;
    c.set_ccr(X);
    c.run(1);
    CHECK(c.ccr() == (X | C | Z));  // D0 = 0 => Z
}

TEST_CASE("shifts de memoria", "[m68k]") {
    Rig r;
    r.code({shmem(1, true)});  // LSL.W (A0)
    r.cpu.a[0] = 0x1000; r.ram.w16(0x1000, 0x8001);
    r.run(1);
    CHECK(r.ram.r16(0x1000) == 0x0002);
    CHECK(r.ccr() == (C | X));

    Rig b;
    b.code({shmem(0, false)});  // ASR.W (A0)
    b.cpu.a[0] = 0x1000; b.ram.w16(0x1000, 0x8000);
    b.run(1);
    CHECK(b.ram.r16(0x1000) == 0xC000);
    CHECK(b.ccr() == N);  // bit 0 era 0: C/X limpos
}

// ------------------------------------------------------------------ MOVEM / LINK / pilha

TEST_CASE("MOVEM com predecremento e postincremento", "[m68k]") {
    Rig r;
    r.code({0x48E7, 0xE080, 0x4CDF, 0x0107});  // MOVEM.L D0-D2/A0,-(A7) ; MOVEM.L (A7)+,D0-D2/A0
    r.cpu.d[0] = 0x11; r.cpu.d[1] = 0x22; r.cpu.d[2] = 0x33; r.cpu.a[0] = 0x44;
    r.run(1);
    CHECK(r.cpu.a[7] == 0x2000u - 16);
    CHECK(r.ram.r32(0x1FF0) == 0x11u);  // D0 no menor endereco
    CHECK(r.ram.r32(0x1FF4) == 0x22u);
    CHECK(r.ram.r32(0x1FF8) == 0x33u);
    CHECK(r.ram.r32(0x1FFC) == 0x44u);
    r.cpu.d[0] = r.cpu.d[1] = r.cpu.d[2] = r.cpu.a[0] = 0;
    r.run(1);
    CHECK(r.cpu.d[0] == 0x11u);
    CHECK(r.cpu.d[1] == 0x22u);
    CHECK(r.cpu.d[2] == 0x33u);
    CHECK(r.cpu.a[0] == 0x44u);
    CHECK(r.cpu.a[7] == 0x2000u);
}

TEST_CASE("MOVEM palavra estende sinal ao carregar; ascendente para memoria", "[m68k]") {
    Rig r;
    r.code({0x4C98, 0x0003});  // MOVEM.W (A0)+,D0-D1
    r.cpu.a[0] = 0x1000;
    r.ram.w16(0x1000, 0x8000); r.ram.w16(0x1002, 0x1234);
    r.run(1);
    CHECK(r.cpu.d[0] == 0xFFFF8000u);
    CHECK(r.cpu.d[1] == 0x1234u);
    CHECK(r.cpu.a[0] == 0x1004u);

    Rig b;
    b.code({0x48D0, 0x0003});  // MOVEM.L D0-D1,(A0)
    b.cpu.a[0] = 0x1000; b.cpu.d[0] = 0xAAAAAAAA; b.cpu.d[1] = 0xBBBBBBBB;
    b.run(1);
    CHECK(b.ram.r32(0x1000) == 0xAAAAAAAAu);
    CHECK(b.ram.r32(0x1004) == 0xBBBBBBBBu);
    CHECK(b.cpu.a[0] == 0x1000u);
}

TEST_CASE("LINK e UNLK", "[m68k]") {
    Rig r;
    r.code({0x4E56, 0xFFF8, 0x4E5E});  // LINK A6,#-8 ; UNLK A6
    r.cpu.a[6] = 0x1111;
    r.run(1);
    CHECK(r.cpu.a[6] == 0x1FFCu);
    CHECK(r.cpu.a[7] == 0x1FF4u);
    CHECK(r.ram.r32(0x1FFC) == 0x1111u);
    r.run(1);
    CHECK(r.cpu.a[7] == 0x2000u);
    CHECK(r.cpu.a[6] == 0x1111u);
}

TEST_CASE("JSR/RTS/JMP/BSR/RTR/PEA", "[m68k]") {
    Rig r;
    r.code({0x4EB9, 0x0000, 0x0800, 0x7401});  // JSR $800.L ; MOVEQ #1,D2
    r.code({0x4E75}, 0x800);                   // RTS
    r.run(1);
    CHECK(r.cpu.pc == 0x800u);
    CHECK(r.ram.r32(0x1FFC) == 0x406u);
    r.run(2);
    CHECK(r.cpu.pc == 0x408u);
    CHECK(r.cpu.d[2] == 1u);
    CHECK(r.cpu.a[7] == 0x2000u);

    Rig b;  // BSR.B +4
    b.code({0x6104, 0x7401, 0x4E71, 0x7605, 0x4E75});
    b.run(4);
    CHECK(b.cpu.d[3] == 5u);
    CHECK(b.cpu.d[2] == 1u);
    CHECK(b.cpu.a[7] == 0x2000u);

    Rig j;
    j.code({0x4ED0});  // JMP (A0)
    j.cpu.a[0] = 0x1234;
    j.run(1);
    CHECK(j.cpu.pc == 0x1234u);

    Rig t;  // RTR restaura CCR e PC
    t.code({0x4E77});
    t.cpu.a[7] = 0x1FF8;
    t.ram.w16(0x1FF8, 0xFF15); t.ram.w32(0x1FFA, 0x700);
    t.run(1);
    CHECK(t.cpu.pc == 0x700u);
    CHECK(t.ccr() == 0x15);
    CHECK((t.cpu.sr() & 0xFF00) == 0x2000);
    CHECK(t.cpu.a[7] == 0x1FFEu);

    Rig p;
    p.code({0x4868, 0x0008});  // PEA 8(A0)
    p.cpu.a[0] = 0x1000;
    p.run(1);
    CHECK(p.cpu.a[7] == 0x1FFCu);
    CHECK(p.ram.r32(0x1FFC) == 0x1008u);
}

TEST_CASE("LEA com todos os modos de controle", "[m68k]") {
    struct Caso { std::initializer_list<uint16_t> prog; uint32_t esperado; };
    const Caso casos[] = {
        {{0x43D0}, 0x1000},                  // (A0)
        {{0x43E8, 0x0004}, 0x1004},          // d16(A0)
        {{0x43E8, 0xFFFE}, 0x0FFE},          // d16 negativo
        {{0x43F8, 0x1234}, 0x1234},          // abs.W
        {{0x43F8, 0x8000}, 0xFFFF8000},      // abs.W com extensao de sinal
        {{0x43F9, 0x0001, 0x2345}, 0x12345},  // abs.L
        {{0x43F0, 0x18F0}, 0x1000},          // (d8=-16,A0,D1.L) com D1=0x10
        {{0x43F0, 0x10FC}, 0x0FFA},          // (d8=-4,A0,D1.W): D1.W = -2
        {{0x43F0, 0xA808}, 0x1108},          // (d8=8,A0,A2.L) com A2=0x100
        {{0x43FA, 0x0010}, 0x412},           // d16(PC)
        {{0x43FB, 0x0004}, 0x426},           // (d8,PC,D0.W) com D0=0x20
    };
    int i = 0;
    for (const auto& c : casos) {
        Rig r;
        r.code(c.prog);
        r.cpu.a[0] = 0x1000; r.cpu.a[2] = 0x100; r.cpu.d[0] = 0x20;
        r.cpu.d[1] = (i == 7) ? 0x0001FFFE : 0x10;
        r.run(1);
        INFO("caso " << i);
        CHECK(r.cpu.a[1] == c.esperado);
        ++i;
    }
}

// ------------------------------------------------------------------ excecoes e modos

TEST_CASE("TRAP/RTE mudam de usuario para supervisor e trocam a pilha", "[m68k]") {
    Rig r;
    r.vector(35, 0x800);
    r.code({0x4E43, 0x4E71});  // TRAP #3
    r.code({0x4E73}, 0x800);   // RTE
    r.cpu.set_ssp(0x2000);
    r.cpu.set_sr(0x0005);      // usuario, Z|C
    r.cpu.set_usp(0x3000);
    r.run(1);
    CHECK((r.cpu.sr() & M68k::kSuper) != 0);
    CHECK(r.cpu.pc == 0x800u);
    CHECK(r.cpu.a[7] == 0x1FFAu);
    CHECK(r.cpu.usp() == 0x3000u);
    CHECK(r.ram.r16(0x1FFA) == 0x0005);
    CHECK(r.ram.r32(0x1FFC) == 0x402u);
    r.run(1);
    CHECK((r.cpu.sr() & M68k::kSuper) == 0);
    CHECK(r.cpu.sr() == 0x0005);
    CHECK(r.cpu.a[7] == 0x3000u);
    CHECK(r.cpu.ssp() == 0x2000u);
    CHECK(r.cpu.pc == 0x402u);
}

TEST_CASE("instrucao ilegal, linha A/F, TRAPV e CHK", "[m68k]") {
    Rig il;
    il.vector(4, 0x800);
    il.code({0x4AFC});
    il.run(1);
    CHECK(il.cpu.pc == 0x800u);
    CHECK(il.ram.r32(0x1FFC) == 0x400u);

    Rig la;
    la.vector(10, 0x800); la.code({0xA000});
    la.run(1);
    CHECK(la.cpu.pc == 0x800u);
    Rig lf;
    lf.vector(11, 0x800); lf.code({0xF123});
    lf.run(1);
    CHECK(lf.cpu.pc == 0x800u);

    Rig tv;
    tv.vector(7, 0x800); tv.code({0x4E76, 0x4E76});
    tv.run(1);
    CHECK(tv.cpu.pc == 0x402u);  // sem V nao ha trap
    tv.set_ccr(V);
    tv.run(1);
    CHECK(tv.cpu.pc == 0x800u);
    CHECK(tv.ram.r32(0x1FFC) == 0x404u);

    Rig ch;
    ch.vector(6, 0x800); ch.code({0x4181});  // CHK D1,D0
    ch.cpu.d[0] = 2; ch.cpu.d[1] = 3;
    ch.run(1);
    CHECK(ch.cpu.pc == 0x402u);
    Rig ch2;
    ch2.vector(6, 0x800); ch2.code({0x4181});
    ch2.cpu.d[0] = 5; ch2.cpu.d[1] = 3;
    ch2.run(1);
    CHECK(ch2.cpu.pc == 0x800u);
    CHECK((ch2.ram.r16(0x1FFA) & N) == 0);
    Rig ch3;
    ch3.vector(6, 0x800); ch3.code({0x4181});
    ch3.cpu.d[0] = 0xFFFF; ch3.cpu.d[1] = 3;
    ch3.run(1);
    CHECK(ch3.cpu.pc == 0x800u);
    CHECK((ch3.ram.r16(0x1FFA) & N) != 0);
}

TEST_CASE("violacao de privilegio em modo usuario", "[m68k]") {
    const std::initializer_list<uint16_t> progs[] = {
        {0x46FC, 0x2700},  // MOVE #imm,SR
        {0x4E72, 0x2000},  // STOP
        {0x4E73},          // RTE
        {0x4E60},          // MOVE A0,USP
        {0x027C, 0xDFFF},  // ANDI #imm,SR
        {0x4E70},          // RESET
    };
    for (const auto& p : progs) {
        Rig r;
        r.vector(8, 0x800);
        r.code(p);
        r.cpu.set_ssp(0x2000);
        r.cpu.set_sr(0x0000);
        r.cpu.set_usp(0x3000);
        r.run(1);
        CHECK(r.cpu.pc == 0x800u);
        CHECK(r.ram.r32(0x1FFC) == 0x400u);
        CHECK(r.ram.r16(0x1FFA) == 0x0000);
    }
}

TEST_CASE("interrupcao respeita a mascara do SR", "[m68k]") {
    Rig r;
    r.vector(27, 0x900);
    r.code({0x4E71, 0x4E71});
    r.code({0x4E73}, 0x900);
    r.cpu.set_sr(0x2300);  // mascara 3
    r.cpu.raise_interrupt(3);
    CHECK(r.cpu.step() == 4);  // nivel 3 <= mascara: executa o NOP
    CHECK(r.cpu.pc == 0x402u);
    r.cpu.set_sr(0x2000);
    CHECK(r.cpu.step() == 44);
    CHECK(r.cpu.pc == 0x900u);
    CHECK((r.cpu.sr() & M68k::kIntMask) == 0x0300);
    CHECK(r.ram.r16(0x1FFA) == 0x2000);
    CHECK(r.ram.r32(0x1FFC) == 0x402u);
    r.run(1);  // RTE
    CHECK(r.cpu.pc == 0x402u);
    CHECK(r.cpu.sr() == 0x2000);

    Rig b;  // nivel maior que a mascara; nivel 7 passa por mascara 7
    b.vector(28, 0x900); b.vector(31, 0xA00);
    b.cpu.set_sr(0x2300);
    b.cpu.raise_interrupt(4);
    b.run(1);
    CHECK(b.cpu.pc == 0x900u);
    b.cpu.set_sr(0x2700);
    b.cpu.raise_interrupt(7);
    b.run(1);
    CHECK(b.cpu.pc == 0xA00u);
}

TEST_CASE("interrupcao a partir do modo usuario usa a pilha de supervisor", "[m68k]") {
    Rig r;
    r.vector(26, 0x900);
    r.code({0x4E71});
    r.cpu.set_ssp(0x2000);
    r.cpu.set_sr(0x0000);
    r.cpu.set_usp(0x3000);
    r.cpu.raise_interrupt(2);
    r.run(1);
    CHECK(r.cpu.pc == 0x900u);
    CHECK(r.cpu.a[7] == 0x1FFAu);
    CHECK(r.cpu.usp() == 0x3000u);
    CHECK(r.ram.r16(0x1FFA) == 0x0000);
}

TEST_CASE("STOP espera por interrupcao", "[m68k]") {
    Rig r;
    r.vector(25, 0x900);
    r.code({0x4E72, 0x2000, 0x4E71});
    r.run(1);
    CHECK(r.cpu.stopped());
    r.run(3);
    CHECK(r.cpu.pc == 0x404u);
    r.cpu.raise_interrupt(1);
    r.run(1);
    CHECK(!r.cpu.stopped());
    CHECK(r.cpu.pc == 0x900u);
    CHECK(r.ram.r32(0x1FFC) == 0x404u);
}

// ------------------------------------------------------------------ SR / CCR / USP

TEST_CASE("MOVE de/para SR, CCR e USP; ANDI/ORI/EORI para CCR e SR", "[m68k]") {
    Rig r;
    r.code({0x40C0, 0x44C0});  // MOVE SR,D0 ; MOVE D0,CCR
    r.set_ccr(Z);
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFFFF) == 0x2004u);
    r.cpu.d[0] = 0xFFFF;
    r.run(1);
    CHECK(r.cpu.sr() == 0x201F);

    Rig b;
    b.code({0x023C, 0x00FE, 0x003C, 0x0011, 0x0A3C, 0x0005});  // ANDI #$FE,CCR ; ORI #$11,CCR ; EORI #5,CCR
    b.set_ccr(0x1F);
    b.run(1);
    CHECK(b.ccr() == 0x1E);
    b.run(1);
    CHECK(b.ccr() == 0x1F);
    b.run(1);
    CHECK(b.ccr() == 0x1A);

    Rig c;
    c.code({0x007C, 0x0700, 0x027C, 0xD8FF});  // ORI #$0700,SR ; ANDI #$D8FF,SR (usuario)
    c.cpu.set_usp(0x3000);
    c.cpu.set_ssp(0x2000);
    c.run(1);
    CHECK(c.cpu.sr() == 0x2700);
    c.run(1);
    CHECK((c.cpu.sr() & M68k::kSuper) == 0);
    CHECK(c.cpu.a[7] == 0x3000u);

    Rig u;
    u.code({0x4E60, 0x4E69});  // MOVE A0,USP ; MOVE USP,A1
    u.cpu.a[0] = 0x4444;
    u.run(2);
    CHECK(u.cpu.usp() == 0x4444u);
    CHECK(u.cpu.a[1] == 0x4444u);
    CHECK(u.cpu.a[7] == 0x2000u);
}

// ------------------------------------------------------------------ bits, BCD, diversos

TEST_CASE("BTST/BSET/BCLR/BCHG em registrador", "[m68k]") {
    Rig r;
    r.code({0x0300, 0x0300, 0x0300});  // BTST D1,D0 x3
    r.cpu.d[0] = 2;
    r.set_ccr(X | N | V | C);
    r.cpu.d[1] = 1;
    r.run(1);
    CHECK(r.ccr() == (X | N | V | C));  // bit 1 setado: Z=0; demais flags intactas
    r.cpu.d[1] = 2;
    r.run(1);
    CHECK((r.ccr() & Z) != 0);
    r.cpu.d[1] = 33;  // modulo 32
    r.run(1);
    CHECK((r.ccr() & Z) == 0);

    Rig b;
    b.code({0x03C0, 0x0380, 0x0340, 0x0840, 0x0003});  // BSET D1,D0 ; BCLR D1,D0 ; BCHG D1,D0 ; BCHG #3,D0
    b.cpu.d[1] = 4;
    b.run(1);
    CHECK(b.cpu.d[0] == 0x10u);
    CHECK((b.ccr() & Z) != 0);
    b.run(1);
    CHECK(b.cpu.d[0] == 0u);
    CHECK((b.ccr() & Z) == 0);
    b.run(1);
    CHECK(b.cpu.d[0] == 0x10u);
    b.run(1);
    CHECK(b.cpu.d[0] == 0x18u);
}

TEST_CASE("BTST/BSET/BCLR em memoria (bit modulo 8)", "[m68k]") {
    Rig r;
    r.code({0x03D0, 0x0810, 0x0007, 0x0890, 0x0000});  // BSET D1,(A0) ; BTST #7,(A0) ; BCLR #0,(A0)
    r.cpu.a[0] = 0x1000; r.cpu.d[1] = 9;
    r.ram.m[0x1000] = 0x81;
    r.run(1);
    CHECK(r.ram.m[0x1000] == 0x83);
    CHECK((r.ccr() & Z) != 0);  // bit 1 estava zerado
    r.run(1);
    CHECK((r.ccr() & Z) == 0);  // bit 7 setado
    r.run(1);
    CHECK(r.ram.m[0x1000] == 0x82);
}

TEST_CASE("ABCD, SBCD e NBCD", "[m68k]") {
    Rig r;
    r.code({0xC101, 0xC101, 0xC101});  // ABCD D1,D0
    r.cpu.d[0] = 0x19; r.cpu.d[1] = 0x28;
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x47u);
    CHECK(r.ccr() == 0);
    r.cpu.d[0] = 0x99; r.cpu.d[1] = 0x01;
    r.set_ccr(Z);
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x00u);
    CHECK(r.ccr() == (Z | C | X));  // Z inalterada porque o resultado e zero
    r.cpu.d[0] = 0x09; r.cpu.d[1] = 0x09;
    r.set_ccr(X);
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x19u);

    Rig b;
    b.code({0x8101, 0x8101, 0x4800});  // SBCD D1,D0 ; SBCD D1,D0 ; NBCD D0
    b.cpu.d[0] = 0x10; b.cpu.d[1] = 0x01;
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0x09u);
    CHECK((b.ccr() & (C | X)) == 0);
    b.cpu.d[0] = 0x00;
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0x99u);
    CHECK((b.ccr() & (C | X)) == (C | X));
    b.cpu.d[0] = 0x01;
    b.set_ccr(0);
    b.run(1);
    CHECK((b.cpu.d[0] & 0xFF) == 0x99u);
    CHECK((b.ccr() & C) != 0);

    Rig m;
    m.code({0xC109});  // ABCD -(A1),-(A0)
    m.cpu.a[0] = 0x1011; m.cpu.a[1] = 0x1001;
    m.ram.m[0x1000] = 0x25; m.ram.m[0x1010] = 0x17;
    m.run(1);
    CHECK(m.ram.m[0x1010] == 0x42);
}

TEST_CASE("TAS, EXT, SWAP e EXG", "[m68k]") {
    Rig r;
    r.code({0x4AC0, 0x4AD0});  // TAS D0 ; TAS (A0)
    r.run(1);
    CHECK((r.cpu.d[0] & 0xFF) == 0x80u);
    CHECK(r.ccr() == Z);
    r.cpu.a[0] = 0x1000; r.ram.m[0x1000] = 0x85;
    r.run(1);
    CHECK(r.ram.m[0x1000] == 0x85);
    CHECK(r.ccr() == N);

    Rig b;
    b.code({0x4880, 0x48C0, 0x4840});  // EXT.W ; EXT.L ; SWAP
    b.cpu.d[0] = 0x12345680;
    b.run(1);
    CHECK(b.cpu.d[0] == 0x1234FF80u);
    CHECK(b.ccr() == N);
    b.cpu.d[0] = 0x00008000;
    b.run(1);
    CHECK(b.cpu.d[0] == 0xFFFF8000u);
    b.cpu.d[0] = 0x12345678;
    b.run(1);
    CHECK(b.cpu.d[0] == 0x56781234u);
    CHECK(b.ccr() == 0);

    Rig e;
    e.code({0xC141, 0xC149, 0xC189});  // EXG D0,D1 ; EXG A0,A1 ; EXG D0,A1
    e.cpu.d[0] = 1; e.cpu.d[1] = 2; e.cpu.a[0] = 3; e.cpu.a[1] = 4;
    e.run(1);
    CHECK(e.cpu.d[0] == 2u); CHECK(e.cpu.d[1] == 1u);
    e.run(1);
    CHECK(e.cpu.a[0] == 4u); CHECK(e.cpu.a[1] == 3u);
    e.run(1);
    CHECK(e.cpu.d[0] == 3u); CHECK(e.cpu.a[1] == 2u);
}

TEST_CASE("MOVEP em ambas as direcoes", "[m68k]") {
    Rig r;
    r.code({0x01C9, 0x0000, 0x0189, 0x0008});  // MOVEP.L D0,0(A1) ; MOVEP.W D0,8(A1)
    r.cpu.a[1] = 0x1000; r.cpu.d[0] = 0x11223344;
    for (int i = 0; i < 16; ++i) r.ram.m[0x1000 + i] = 0xEE;
    r.run(2);
    CHECK(r.ram.m[0x1000] == 0x11);
    CHECK(r.ram.m[0x1002] == 0x22);
    CHECK(r.ram.m[0x1004] == 0x33);
    CHECK(r.ram.m[0x1006] == 0x44);
    CHECK(r.ram.m[0x1001] == 0xEE);
    CHECK(r.ram.m[0x1008] == 0x33);
    CHECK(r.ram.m[0x100A] == 0x44);

    Rig b;
    b.code({0x0549, 0x0000, 0x0109, 0x0000});  // MOVEP.L 0(A1),D2 ; MOVEP.W 0(A1),D0
    b.cpu.a[1] = 0x1000; b.cpu.d[0] = 0xFFFF0000;
    b.fill(0x1000, {0x11, 0xEE, 0x22, 0xEE, 0x33, 0xEE, 0x44});
    b.run(2);
    CHECK(b.cpu.d[2] == 0x11223344u);
    CHECK(b.cpu.d[0] == 0xFFFF1122u);
}

TEST_CASE("Scc, NOP e ciclos aproximados", "[m68k]") {
    Rig r;
    r.code({0x4E71, 0x3010, 0x6002, 0x4E71});
    r.cpu.a[0] = 0x1000;
    CHECK(r.cpu.step() == 4);   // NOP
    CHECK(r.cpu.step() == 8);   // MOVE.W (A0),D0
    CHECK(r.cpu.step() == 10);  // BRA.B tomado
}

// ------------------------------------------------------------------ integracao

TEST_CASE("integracao: laco preenche vetor e soma", "[m68k]") {
    Rig r;
    r.code({
        0x41F8, 0x1000,          // 400 LEA $1000.W,A0
        0x700F,                  // 404 MOVEQ #15,D0
        0x7200,                  // 406 MOVEQ #0,D1
        0x30C1,                  // 408 MOVE.W D1,(A0)+
        0x5641,                  // 40A ADDQ.W #3,D1
        0x51C8, 0xFFFA,          // 40C DBF D0,408
        0x41F8, 0x1000,          // 410 LEA $1000.W,A0
        0x700F,                  // 414 MOVEQ #15,D0
        0x7400,                  // 416 MOVEQ #0,D2
        0xD458,                  // 418 ADD.W (A0)+,D2
        0x51C8, 0xFFFC,          // 41A DBF D0,418
        0x31C2, 0x1100,          // 41E MOVE.W D2,$1100.W
        0x4E72, 0x2700,          // 422 STOP #$2700
    });
    int passos = 0;
    while (!r.cpu.stopped() && passos < 1000) { r.cpu.step(); ++passos; }
    REQUIRE(r.cpu.stopped());
    uint32_t esperado = 0;
    for (int i = 0; i < 16; ++i) {
        CHECK(r.ram.r16(0x1000 + 2 * i) == 3 * i);
        esperado += 3 * i;
    }
    CHECK(esperado == 360u);
    CHECK(r.ram.r16(0x1100) == 360);
    CHECK((r.cpu.d[2] & 0xFFFF) == 360u);
}
