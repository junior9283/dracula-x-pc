#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <vector>

#include "saturn/scu.hpp"
#include "saturn/scu_dsp.hpp"

using saturn::Scu;

namespace {
constexpr uint32_t kPpaf = 0x80, kPpd = 0x84, kPda = 0x88, kPdd = 0x8C, kIms = 0xA0;
constexpr uint32_t kExecute = 1u << 16, kPcLoad = 1u << 15, kEndFlag = 1u << 18;

// Codificação das instruções usadas nos testes.
constexpr uint32_t mov_imm(uint32_t d, uint8_t imm) { return 0x1000 | d << 8 | imm; }        // MOV SImm,[d]
constexpr uint32_t mov_src(uint32_t d, uint32_t s) { return 0x3000 | d << 8 | s; }           // MOV [s],[d]
constexpr uint32_t mvi(uint32_t d, uint32_t imm) { return 0x8000'0000 | d << 26 | (imm & 0x1FFFFFF); }
constexpr uint32_t dma_read(uint32_t ram, uint32_t count) { return 0xC000'0000 | 2u << 15 | ram << 8 | count; }
constexpr uint32_t dma_write(uint32_t ram, uint32_t count, uint32_t add_mode = 2) {   // passo = 2^add_mode bytes
    return 0xC000'0000 | 1u << 12 | add_mode << 15 | ram << 8 | count;
}
constexpr uint32_t kNop = 0, kEnd = 0xF000'0000, kEndi = 0xF800'0000, kLps = 0xE800'0000;
constexpr uint32_t kCt0 = 0xC, kCt1 = 0xD, kRa0 = 6, kWa0 = 7, kLop = 0xA, kMc0 = 0, kSrcMc0 = 4, kSrcAll = 9;
constexpr uint32_t kMovM0ToA = 3u << 17 | 0u << 14, kMovM1ToP = 3u << 23 | 1u << 20, kAdd = 4u << 26, kAluToA = 2u << 17;

struct DspRig {
    saturn::Bus bus;
    Scu scu{bus};
    int end_interrupts = 0;

    DspRig() {
        saturn::map_standard_memory(bus);
        scu.deliver = [this](int, int vector) { if (vector == 0x45) ++end_interrupts; };
        scu.write(kIms, 4, 0);
    }
    void load(std::initializer_list<uint32_t> program) {
        scu.write(kPpaf, 4, kPcLoad);
        for (uint32_t i : program) scu.write(kPpd, 4, i);
    }
    void set_data(uint8_t address, std::initializer_list<uint32_t> words) {
        scu.write(kPda, 4, address);
        for (uint32_t w : words) scu.write(kPdd, 4, w);
    }
    uint32_t data(uint8_t address) {
        scu.write(kPda, 4, address);
        return scu.read(kPdd, 4);
    }
    // Inicia em 0 e roda até o fim (com limite); devolve se terminou.
    bool run() {
        scu.write(kPpaf, 4, kExecute | kPcLoad);
        for (int i = 0; i < 10000 && scu.dsp().running(); ++i) scu.advance(64);
        return !scu.dsp().running();
    }
};
}  // namespace

TEST_CASE("DSP do SCU: programa carregado por PPD roda até END e libera PPAF.EX") {
    DspRig rig;
    rig.load({mov_imm(kCt0, 0), mov_imm(kMc0, 0x12), mov_imm(kMc0, -3), kEnd, kNop});
    REQUIRE(rig.run());
    CHECK((rig.scu.read(kPpaf, 4) & kExecute) == 0);
    CHECK(rig.data(0) == 0x12u);
    CHECK(rig.data(1) == 0xFFFFFFFDu);   // imediato de 8 bits com sinal
    CHECK(rig.end_interrupts == 0);
}

TEST_CASE("DSP do SCU: ENDI liga o flag E (limpo na leitura) e pede a interrupção DSP end") {
    DspRig rig;
    rig.load({kEndi, kNop});
    REQUIRE(rig.run());
    CHECK(rig.end_interrupts == 1);
    CHECK((rig.scu.read(kPpaf, 4) & kEndFlag) != 0);
    CHECK((rig.scu.read(kPpaf, 4) & kEndFlag) == 0);
}

TEST_CASE("DSP do SCU: DMA lê da Work RAM para a RAM de dados e grava de volta (uso do BIOS)") {
    DspRig rig;
    for (uint32_t i = 0; i < 4; ++i) rig.bus.write32(0x06001000 + 4 * i, 0xA0B0C000 + i);
    // Parâmetros como o BIOS: endereços em palavras na RAM de dados (banco 0).
    rig.set_data(0, {0x06001000 >> 2, 0x06002000 >> 2});
    rig.load({mov_imm(kCt0, 0), mov_src(kRa0, kSrcMc0), mov_src(kWa0, kSrcMc0), mov_imm(kCt1, 0),
              dma_read(1, 4), mov_imm(kCt1, 0), dma_write(1, 4), kEnd, kNop});
    REQUIRE(rig.run());
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(rig.data(64 + i) == 0xA0B0C000 + i);
        CHECK(rig.bus.read32(0x06002000 + 4 * i) == 0xA0B0C000 + i);
    }
}

TEST_CASE("DSP do SCU: DMA para o B-bus grava em meias palavras, cada uma somando o passo") {
    DspRig rig;
    rig.bus.map_ram(0x05C00000, 0x80000, 0x80000);   // VRAM do VDP1 (B-bus)
    rig.set_data(0, {0x11223344, 0x55667788});
    rig.load({mvi(kWa0, 0x05C00000 >> 2), mov_imm(kCt0, 0), dma_write(0, 2, 1), kEnd, kNop});
    REQUIRE(rig.run());
    CHECK(rig.bus.read32(0x05C00000) == 0x11223344u);
    CHECK(rig.bus.read32(0x05C00004) == 0x55667788u);
}

TEST_CASE("DSP do SCU: ADD soma A e P, MOV ALL grava o resultado e LPS repete a instrução seguinte") {
    DspRig rig;
    rig.set_data(0, {100});
    rig.set_data(64, {23});
    // A = M0 (100); P = M1 (23); A = A + P; M2 = ALL; depois LPS com LOP = 3 grava 4 vezes em M3.
    rig.load({mov_imm(kCt0, 0), kMovM0ToA | kMovM1ToP, kAdd | kAluToA, kAdd | mov_src(2, kSrcAll),
              mvi(kLop, 3), kLps, mov_imm(3, 7), kEnd, kNop});
    REQUIRE(rig.run());
    CHECK(rig.data(128) == 146u);   // ALU de 'kAdd | kAluToA' (123) + P (23) na instrução seguinte
    for (uint8_t i = 0; i < 4; ++i) CHECK(rig.data(192 + i) == 7u);
    CHECK(rig.data(196) == 0u);
}

TEST_CASE("DSP do SCU: salto tem um slot de atraso") {
    DspRig rig;
    rig.load({mov_imm(kCt0, 0), 0xD000'0005, mov_imm(kMc0, 1), mov_imm(kMc0, 2), kNop, mov_imm(kMc0, 3), kEnd, kNop});
    REQUIRE(rig.run());
    CHECK(rig.data(0) == 1u);   // slot de atraso executa
    CHECK(rig.data(1) == 3u);   // a instrução 3 é pulada
}

TEST_CASE("DSP do SCU: estado salvo e carregado continua a execução igual") {
    DspRig a;
    a.set_data(0, {5});
    a.load({mov_imm(kCt0, 0), mvi(kLop, 200), kLps, mov_imm(1, 9), kEnd, kNop});
    a.scu.write(kPpaf, 4, kExecute | kPcLoad);
    a.scu.advance(100);   // para no meio do laço
    saturn::StateWriter w;
    a.scu.save(w);
    const std::vector<uint8_t> bytes = w.take();

    DspRig b;
    saturn::StateReader r(bytes);
    b.scu.load(r);
    for (DspRig* rig : {&a, &b})
        for (int i = 0; i < 100 && rig->scu.dsp().running(); ++i) rig->scu.advance(64);
    for (uint8_t i = 0; i < 64; ++i) CHECK(a.data(64 + i) == b.data(64 + i));
    CHECK_FALSE(b.scu.dsp().running());
}

TEST_CASE("DSP do SCU: estado com CT ou contagem de DMA de programa fora dos limites é recusado ao carregar") {
    saturn::Bus bus;
    saturn::ScuDsp dsp(bus);
    saturn::StateWriter w;
    dsp.save(w);
    const std::vector<uint8_t> good = w.take();

    const auto load = [&](const std::vector<uint8_t>& bytes) {
        saturn::ScuDsp target(bus);
        saturn::StateReader r(bytes);
        target.load(r);
    };
    // Layout: 256 palavras de programa + 4 bancos de 64 palavras, depois CT[4] (bytes) e, no fim, a contagem do DMA de programa (u32).
    constexpr size_t kCtOffset = 4 * (256 + 4 * 64);
    const auto patched = [&](size_t offset, auto write) {
        saturn::StateWriter field;
        write(field);
        const std::vector<uint8_t> raw = field.take();
        std::vector<uint8_t> bytes = good;
        std::copy(raw.begin(), raw.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return bytes;
    };

    CHECK_NOTHROW(load(good));
    CHECK_NOTHROW(load(patched(kCtOffset, [](auto& f) { f.u8(63); })));
    CHECK_THROWS_AS(load(patched(kCtOffset, [](auto& f) { f.u8(64); })), saturn::StateError);
    CHECK_THROWS_AS(load(patched(kCtOffset + 3, [](auto& f) { f.u8(0xFF); })), saturn::StateError);
    CHECK_NOTHROW(load(patched(good.size() - 4, [](auto& f) { f.u32(256); })));
    CHECK_THROWS_AS(load(patched(good.size() - 4, [](auto& f) { f.u32(257); })), saturn::StateError);
    CHECK_THROWS_AS(load(patched(good.size() - 4, [](auto& f) { f.u32(0xFFFFFFFF); })), saturn::StateError);
}
