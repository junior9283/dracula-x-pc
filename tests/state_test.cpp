#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "iso_builder.hpp"
#include "saturn/machine.hpp"
#include "saturn/state.hpp"

using namespace testsupport;
using saturn::Machine;
using saturn::StateReader;
using saturn::StateWriter;

namespace {
constexpr uint32_t kCounter = 0x06004000, kLoop = 0x06002100;
constexpr uint32_t kCdCr1 = 0x05890018, kCdCr2 = 0x0589001C, kCdCr3 = 0x05890020, kCdCr4 = 0x05890024;

// Disco sintético: IP com um laço que incrementa um contador na Work RAM e mexe nos registradores do SMPC/SCU/FRT.
std::unique_ptr<saturnfs::DiscImage> make_disc(const std::string& tag, const char* product = "T-9527G") {
    SyntheticIso iso(product, "V1.400");
    iso.add("A.TXT", "alfa").add("B.BIN", std::string(9000, 'b'));
    auto sectors = iso.sectors();
    sectors[0][0xE2] = 0x18;
    // +0x100: mov.l @(1,pc),r1 não é necessário; o teste põe r1 e o PC direto. Código do laço copiado em kLoop pelo teste.
    return std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(write_single_track_disc(make_temp_dir(tag), sectors)));
}

// Laço: mov.l @r1,r0 ; add #1,r0 ; mov.l r0,@r1 ; add #3,r2 ; bra laço ; nop
void write_counter_loop(Machine& m) {
    const uint16_t code[] = {0x6012, 0x7001, 0x2102, 0x7203, 0xAFFB, 0x0009};
    for (size_t i = 0; i < std::size(code); ++i) m.bus().write16(kLoop + 2 * i, code[i]);
    m.master().state().pc = kLoop;
    m.master().state().r[1] = kCounter;
}

void cd_command(Machine& m, uint16_t c1, uint16_t c2 = 0, uint16_t c3 = 0, uint16_t c4 = 0) {
    m.bus().write16(kCdCr1, c1); m.bus().write16(kCdCr2, c2); m.bus().write16(kCdCr3, c3); m.bus().write16(kCdCr4, c4);
}

// Liga o 68000 do SCSP (laço `bra .`), um slot de ruído tocando e a saída de áudio.
void start_sound(Machine& m) {
    auto& scsp = m.scsp();
    scsp.write_ram(0, 4, 0x1000);        // SSP
    scsp.write_ram(4, 4, 8);             // PC
    scsp.write_ram(8, 2, 0x60FE);        // bra .
    scsp.set_audio_enabled(true);
    scsp.write_reg(0x08, 2, 0x001F);     // taxas: ataque máximo
    scsp.write_reg(0x16, 2, 0xE000);     // DISDL máximo, centro
    scsp.write_reg(0x06, 2, 0xFFFF);     // fim do laço
    scsp.write_reg(0x00, 2, 0x1800 | (1 << 7) | (1 << 5));   // KYONEX + KYONB, SSCTL = ruído, laço normal
    scsp.power(true);
}

// Máquina com atividade em todos os componentes, depois de `frames` quadros.
void warm_up(Machine& m, int frames) {
    write_counter_loop(m);
    m.smpc().set_pad(0xF7FF);
    m.scu().write(0xA0, 4, 0);           // IMS: todas as interrupções liberadas (sem tratador: o trampolim só retorna)
    m.master().onchip().write(0x16, 1, 0x00);   // FRT em andamento
    cd_command(m, 0x4600 | 1, 0, 0);     // SetFilterConnection: filtro 0, saída verdadeira = partição 0
    cd_command(m, 0x3000, 0, 0);         // SetConnection: filtro 0
    cd_command(m, 0x1080, 150, 0x0080, 5);   // Play de 5 setores a partir de FAD 150 (vão ao buffer)
    start_sound(m);
    for (int i = 0; i < frames; ++i) m.run_frame();
}

std::vector<int16_t> drain(Machine& m) {
    std::vector<int16_t> all, chunk(4096);
    while (const size_t n = m.scsp().drain_audio(chunk)) all.insert(all.end(), chunk.begin(), chunk.begin() + n);
    return all;
}
}  // namespace

TEST_CASE("StateWriter/StateReader: ida e volta, seções e leitura além do fim") {
    StateWriter w;
    w.u8(0xAB); w.boolean(true); w.u16(0x1234); w.u32(0xDEADBEEF); w.u64(0x0102030405060708ull); w.i16(-5); w.i32(-70000); w.i64(-1);
    w.text("olá");
    w.section("ABCD", [&] { w.u32(7); w.u8(9); });
    const auto bytes = w.take();

    StateReader r(bytes);
    CHECK(r.u8() == 0xAB); CHECK(r.boolean()); CHECK(r.u16() == 0x1234); CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0102030405060708ull); CHECK(r.i16() == -5); CHECK(r.i32() == -70000); CHECK(r.i64() == -1);
    CHECK(r.text() == "olá");
    r.section("ABCD", [&](StateReader& s) { CHECK(s.u32() == 7); CHECK(s.u8() == 9); });
    CHECK(r.remaining() == 0);
    CHECK_THROWS_AS(r.u8(), saturn::StateError);
}

TEST_CASE("StateReader recusa tag errada, sobra na seção, tamanho absurdo e booleano inválido") {
    StateWriter w;
    w.section("ABCD", [&] { w.u32(1); w.u32(2); });
    const auto bytes = w.take();
    { StateReader r(bytes); CHECK_THROWS_AS(r.section("WXYZ", [](StateReader&) {}), saturn::StateError); }
    { StateReader r(bytes); CHECK_THROWS_AS(r.section("ABCD", [](StateReader& s) { s.u32(); }), saturn::StateError); }  // sobra
    { StateReader r(bytes); CHECK_THROWS_AS(r.section("ABCD", [](StateReader& s) { s.u64(); s.u8(); }), saturn::StateError); }  // além do fim
    { StateReader r(std::span<const uint8_t>(bytes).first(bytes.size() - 1)); CHECK_THROWS_AS(r.section("ABCD", [](StateReader&) {}), saturn::StateError); }
    const std::vector<uint8_t> big = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    { StateReader r(big); CHECK_THROWS_AS(r.blob(), saturn::StateError); }
    const std::vector<uint8_t> two = {2};
    { StateReader r(two); CHECK_THROWS_AS(r.boolean(), saturn::StateError); }
}

TEST_CASE("save state: ida e volta restaura cada componente") {
    Machine m(make_disc("st1"));
    m.boot();
    warm_up(m, 6);
    const auto saved = m.save_state();

    // Valores de referência no instante do save.
    const auto master = m.master().state();
    const auto slave_pc = m.slave().state().pc;
    const uint64_t frame = m.frame();
    const uint32_t counter = m.bus().read32(kCounter);
    const uint32_t ims = m.scu().read(0xA0, 4), ist = m.scu().read(0xA4, 4);
    const uint32_t hirq = m.bus().read16(0x05890008);
    const uint32_t frc = m.master().onchip().read(0x12, 2);
    const auto m68k_pc = m.scsp().cpu().pc;
    const uint32_t scipd = m.scsp().read_reg(0x420, 2);
    CHECK(counter > 0);
    CHECK((hirq & saturn::CdBlock::kCsct) != 0);   // o Play entregou setores
    CHECK(m.scsp().running());

    // Bagunça tudo e carrega de volta.
    for (int i = 0; i < 3; ++i) m.run_frame();
    m.smpc().set_pad(0x0000);
    m.bus().write32(0x25C00000, 0x12345678);   // VDP1 VRAM
    m.bus().write16(0x25F00010, 0xBEEF);       // CRAM
    m.scsp().write_ram(0x1234, 4, 0xCAFEBABE);
    m.load_state(saved);

    CHECK(m.frame() == frame);
    CHECK(m.master().state().pc == master.pc);
    CHECK(m.master().state().sr == master.sr);
    for (int i = 0; i < 16; ++i) CHECK(m.master().state().r[i] == master.r[i]);
    CHECK(m.slave().state().pc == slave_pc);
    CHECK(m.bus().read32(kCounter) == counter);
    CHECK(m.bus().read32(0x25C00000) != 0x12345678);
    CHECK(m.bus().read16(0x25F00010) != 0xBEEF);
    CHECK(m.scsp().read_ram(0x1234, 4) != 0xCAFEBABE);
    CHECK(m.scu().read(0xA0, 4) == ims);
    CHECK(m.scu().read(0xA4, 4) == ist);
    CHECK(m.bus().read16(0x05890008) == hirq);
    CHECK(m.master().onchip().read(0x12, 2) == frc);
    CHECK(m.scsp().cpu().pc == m68k_pc);
    CHECK(m.scsp().read_reg(0x420, 2) == scipd);
    CHECK(m.save_state() == saved);   // tudo igual byte a byte
}

TEST_CASE("save state: continuar depois do load reproduz exatamente a execução contínua") {
    Machine m(make_disc("st2"));
    m.boot();
    warm_up(m, 4);
    drain(m);
    const auto saved = m.save_state();

    for (int i = 0; i < 8; ++i) m.run_frame();
    const auto continuous_state = m.save_state();
    const auto continuous_audio = drain(m);
    CHECK_FALSE(continuous_audio.empty());

    m.load_state(saved);
    for (int i = 0; i < 8; ++i) m.run_frame();
    CHECK(m.save_state() == continuous_state);
    CHECK(drain(m) == continuous_audio);

    // Também numa máquina nova (outra instância, mesmo disco e modo de boot).
    Machine other(make_disc("st2b"));
    other.boot();
    other.scsp().set_audio_enabled(true);   // a saída de áudio é configuração do consumidor, não faz parte do estado
    other.load_state(saved);
    for (int i = 0; i < 8; ++i) other.run_frame();
    CHECK(other.save_state() == continuous_state);
    CHECK(drain(other) == continuous_audio);
}

TEST_CASE("save state: recusa versão, disco, modo de boot, truncamento e corrupção sem alterar a máquina") {
    Machine m(make_disc("st3"));
    m.boot();
    warm_up(m, 2);
    const auto saved = m.save_state();
    m.run_frame();   // a máquina agora difere do save
    const auto before = m.save_state();
    REQUIRE(before != saved);

    const auto expect_refused = [&](const std::vector<uint8_t>& bad) {
        CHECK_THROWS_AS(m.load_state(bad), std::runtime_error);
        CHECK(m.save_state() == before);
    };

    auto bad = saved; bad[0] ^= 0xFF; expect_refused(bad);                    // assinatura
    bad = saved; bad[8] = 99; expect_refused(bad);                            // versão
    bad = saved; bad[12] ^= 0x01; expect_refused(bad);                        // nº de setores do disco
    bad = saved; bad[16 + 2] ^= 0x01; expect_refused(bad);                    // ID do produto
    bad = saved; bad[32] ^= 0x01; expect_refused(bad);                        // hash do setor 0
    bad = saved; bad[40] ^= 0x02; expect_refused(bad);                        // modo de boot
    bad = saved; bad[bad.size() / 2] ^= 0x10; expect_refused(bad);            // corrupção no conteúdo (checksum)
    bad = saved; bad.resize(bad.size() - 1); expect_refused(bad);             // truncado
    bad = saved; bad.resize(100); expect_refused(bad);
    bad = saved; bad.push_back(0); expect_refused(bad);                       // sobra
    expect_refused({});

    // Outro disco (ID de produto diferente).
    Machine other(make_disc("st3b", "T-1234G"));
    other.boot();
    CHECK_THROWS_AS(other.load_state(saved), std::runtime_error);
    // Máquina sem boot não aceita estado de uma máquina inicializada.
    Machine unbooted(make_disc("st3c"));
    CHECK_THROWS_AS(unbooted.load_state(saved), std::runtime_error);

    // Um estado válido ainda carrega depois das recusas.
    m.load_state(saved);
    CHECK(m.save_state() == saved);
}

TEST_CASE("save state: falha no meio da aplicação restaura a máquina") {
    // Conteúdo com checksum válido mas com a última seção (SCSP) adulterada: as anteriores já foram aplicadas quando a falha acontece.
    Machine m(make_disc("st4"));
    m.boot();
    warm_up(m, 2);
    const auto good = m.save_state();
    m.run_frame();
    const auto before = m.save_state();
    REQUIRE(before != good);

    constexpr size_t kPayloadOffset = 8 + 4 + 4 + 16 + 8 + 1 + 8 + 8, kChecksumOffset = kPayloadOffset - 8;
    auto bad = good;
    const std::string tag = "SCSP";
    const auto last = std::find_end(bad.begin() + kPayloadOffset, bad.end(), tag.begin(), tag.end());
    REQUIRE(last != bad.end());
    *last = 'X';
    const uint64_t sum = saturn::state_checksum(std::span<const uint8_t>(bad).subspan(kPayloadOffset));
    for (int i = 0; i < 8; ++i) bad[kChecksumOffset + i] = static_cast<uint8_t>(sum >> (8 * i));

    CHECK_THROWS_AS(m.load_state(bad), std::runtime_error);
    CHECK(m.save_state() == before);
}
