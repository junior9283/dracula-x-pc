#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "saturn/scu.hpp"
#include "saturn/video_timing.hpp"

using saturn::Scu;
using saturn::VideoTiming;

namespace {
constexpr uint32_t kImsOffset = 0xA0, kIstOffset = 0xA4;
constexpr uint32_t kT0c = 0x90;

struct Delivered { int level, vector; };

struct ScuRig {
    saturn::Bus bus;
    Scu scu{bus};
    std::vector<Delivered> delivered;

    ScuRig() {
        saturn::map_standard_memory(bus);
        scu.deliver = [this](int level, int vector) { delivered.push_back({level, vector}); };
        scu.write(kImsOffset, 4, 0);  // o SCU reinicia com tudo mascarado; os testes desmascaram tudo
    }
};
}  // namespace

TEST_CASE("interrupção mascarada fica pendente em IST e é entregue ao desmascarar") {
    ScuRig rig;
    rig.scu.write(kImsOffset, 4, 0xFFFFFFFF);          // tudo mascarado
    rig.scu.raise(Scu::Interrupt::VBlankIn);

    CHECK(rig.delivered.empty());
    CHECK((rig.scu.read(kIstOffset, 4) & 1) == 1u);

    rig.scu.write(kImsOffset, 4, 0xFFFFFFFE);          // desmascara o bit 0
    REQUIRE_FALSE(rig.delivered.empty());
    CHECK(rig.delivered.back().level == 0xF);
    CHECK(rig.delivered.back().vector == 0x40);
}

TEST_CASE("prioridade: VBlank-IN (nível F) antes de HBlank-IN (nível D); o acknowledge libera a próxima") {
    ScuRig rig;
    rig.scu.raise(Scu::Interrupt::HBlankIn);
    rig.scu.raise(Scu::Interrupt::VBlankIn);

    REQUIRE_FALSE(rig.delivered.empty());
    CHECK(rig.delivered.back().vector == 0x40);
    rig.scu.acknowledge(0x40);
    CHECK((rig.scu.read(kIstOffset, 4) & 1) == 0u);    // o acknowledge limpa o bit
    CHECK(rig.delivered.back().vector == 0x42);
    CHECK(rig.delivered.back().level == 0xD);
}

TEST_CASE("níveis e vetores padrão das fontes do SCU") {
    struct Case { Scu::Interrupt source; int level, vector; };
    for (Case k : {Case{Scu::Interrupt::VBlankOut, 0xE, 0x41}, Case{Scu::Interrupt::Timer0, 0xC, 0x43}, Case{Scu::Interrupt::Timer1, 0xB, 0x44},
                   Case{Scu::Interrupt::SoundRequest, 9, 0x46}, Case{Scu::Interrupt::SystemManager, 8, 0x47}, Case{Scu::Interrupt::Pad, 8, 0x48},
                   Case{Scu::Interrupt::Dma0End, 5, 0x4B}, Case{Scu::Interrupt::SpriteEnd, 2, 0x4D}}) {
        ScuRig rig;
        rig.scu.raise(k.source);
        REQUIRE_FALSE(rig.delivered.empty());
        CHECK(rig.delivered.back().level == k.level);
        CHECK(rig.delivered.back().vector == k.vector);
    }
}

TEST_CASE("escrever 0 em IST limpa o bit pendente") {
    ScuRig rig;
    rig.scu.write(kImsOffset, 4, 0xFFFFFFFF);
    rig.scu.raise(Scu::Interrupt::Pad);
    CHECK((rig.scu.read(kIstOffset, 4) & (1u << 8)) != 0);
    rig.scu.write(kIstOffset, 4, ~(1u << 8));
    CHECK((rig.scu.read(kIstOffset, 4) & (1u << 8)) == 0);
}

TEST_CASE("DMA nível 0 em modo direto copia N bytes e gera a interrupção de fim") {
    ScuRig rig;
    for (uint32_t i = 0; i < 16; i += 4) rig.bus.write32(0x06000100 + i, 0x11111111u * (i / 4 + 1));
    rig.scu.write(0x00, 4, 0x06000100);   // D0R: origem
    rig.scu.write(0x04, 4, 0x06000200);   // D0W: destino
    rig.scu.write(0x08, 4, 16);           // D0C: 16 bytes
    rig.scu.write(0x0C, 4, 0x0102);       // D0AD: leitura +4, escrita +4
    rig.scu.write(0x14, 4, 0x7);          // D0MD: direto, fator de início por software
    rig.scu.write(0x10, 4, 0x101);        // D0EN: habilita e dispara

    for (uint32_t i = 0; i < 16; i += 4) CHECK(rig.bus.read32(0x06000200 + i) == 0x11111111u * (i / 4 + 1));
    REQUIRE_FALSE(rig.delivered.empty());
    CHECK(rig.delivered.back().vector == 0x4B);
}

TEST_CASE("Timer 0 dispara quando a contagem de HBlank-IN alcança T0C e zera no VBlank-OUT") {
    ScuRig rig;
    auto hblank = [&] { rig.scu.raise(Scu::Interrupt::HBlankIn); rig.scu.acknowledge(0x42); };  // um tratador real reconhece cada HBlank
    auto timer0_delivered = [&](size_t from) {
        for (size_t i = from; i < rig.delivered.size(); ++i) if (rig.delivered[i].vector == 0x43) return true;
        return false;
    };

    rig.scu.write(kT0c, 4, 3);
    hblank();
    hblank();
    CHECK_FALSE(timer0_delivered(0));
    hblank();                                    // terceira linha: a contagem alcança T0C
    CHECK(timer0_delivered(0));

    rig.scu.raise(Scu::Interrupt::VBlankOut);    // reinicia a contagem
    rig.scu.acknowledge(0x41);
    rig.scu.acknowledge(0x43);
    const auto before = rig.delivered.size();
    hblank();
    CHECK_FALSE(timer0_delivered(before));       // contagem 1, não 3
}

namespace {
struct TimingRig {
    saturn::Bus bus;
    Scu scu{bus};
    VideoTiming timing{scu};
    int vblank_in = 0, vblank_out = 0, hblank_in = 0;

    TimingRig() {
        scu.write(kImsOffset, 4, 0);
        scu.deliver = [this](int, int vector) {
            if (vector == 0x40) { ++vblank_in; scu.acknowledge(0x40); }
            if (vector == 0x41) { ++vblank_out; scu.acknowledge(0x41); }
            if (vector == 0x42) { ++hblank_in; scu.acknowledge(0x42); }
        };
    }
};
}  // namespace

TEST_CASE("um quadro NTSC tem 262 linhas: 1 VBlank-IN, 1 VBlank-OUT e 262 HBlank-IN") {
    TimingRig rig;
    rig.timing.advance(VideoTiming::kCyclesPerFrame);

    CHECK(rig.vblank_in == 1);
    CHECK(rig.vblank_out == 1);
    CHECK(rig.hblank_in == 262);
    CHECK(rig.timing.frame() == 1);
}

TEST_CASE("TVSTAT reflete VBLANK (bit 3) e HBLANK (bit 2) no instante certo") {
    TimingRig rig;
    rig.timing.advance(10 * VideoTiming::kCyclesPerLine + 100);
    CHECK((rig.timing.tvstat() & 0x0C) == 0);            // linha ativa, antes do HBlank

    rig.timing.advance(VideoTiming::kHBlankStart);       // já dentro do HBlank da linha 10
    CHECK((rig.timing.tvstat() & 0x04) != 0);

    TimingRig vb;
    vb.timing.advance(224 * VideoTiming::kCyclesPerLine + 10);
    CHECK((vb.timing.tvstat() & 0x08) != 0);              // VBlank a partir da linha 224
}

TEST_CASE("TVSTAT é legível no mapa de E/S (VDP2 + 0x04)") {
    TimingRig rig;
    rig.timing.advance(224 * VideoTiming::kCyclesPerLine + 10);
    CHECK((rig.timing.read(0x04, 2) & 0x08) != 0);
    CHECK(rig.timing.read(0x00, 2) == 0u);
}

TEST_CASE("VideoTiming guarda os registradores do VDP2 (exceto TVSTAT, que reflete o tempo)") {
    TimingRig rig;
    rig.timing.write(0x00, 2, 0x8001);    // TVMD
    rig.timing.write(0x04, 2, 0xFFFF);    // TVSTAT é somente leitura: a escrita não vale
    CHECK(rig.timing.read(0x00, 2) == 0x8001u);
    CHECK((rig.timing.read(0x04, 2) & 0x0C) == 0u);
}
