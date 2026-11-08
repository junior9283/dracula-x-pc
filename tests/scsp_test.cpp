#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "audio/scsp.hpp"
#include "iso_builder.hpp"
#include "saturn/machine.hpp"
#include "saturn/state.hpp"

using audio::Scsp;

namespace {
constexpr uint32_t kSh2Ram = 0x25A00000, kSh2Registers = 0x25B00000, kSmpc = 0x20100000;
constexpr uint32_t kSmpcComreg = 0x1F, kSmpcSf = 0x63;
constexpr uint8_t kSndOn = 0x06, kSndOff = 0x07;
constexpr uint32_t kMasterHz = 26'846'590;
constexpr uint32_t kOneSample = 609;   // ciclos do mestre por amostra (>= 26846590 / 44100)

void advance_samples(Scsp& scsp, int samples) { for (int i = 0; i < samples; ++i) scsp.advance(kOneSample, kMasterHz); }

// Vetores iniciais (SSP, PC) mais o código, em `ram` do SCSP/da RAM de som.
void put_words(std::vector<uint8_t>& image, uint32_t at, std::initializer_list<uint16_t> words) {
    for (uint16_t w : words) { image[at++] = w >> 8; image[at++] = w & 0xFF; }
}

// Programa: libera a máscara de interrupção e fica em laço; o tratador do nível 2 (vetor 0x68 -> 0x1100) grava 0x00AA em 0x800 e volta.
std::vector<uint8_t> interrupt_program() {
    std::vector<uint8_t> image(0x1200, 0);
    put_words(image, 0, {0x0000, 0xA000, 0x0000, 0x1000});                  // SSP = 0xA000, PC = 0x1000
    put_words(image, 0x68, {0x0000, 0x1100});                              // autovetor do nível 2
    put_words(image, 0x1000, {0x46FC, 0x2000, 0x60FE});                    // move #$2000,sr ; bra.s .
    put_words(image, 0x1100, {0x33FC, 0x00AA, 0x0000, 0x0800, 0x4E73});    // move.w #$AA,$800.l ; rte
    return image;
}

void load(Scsp& scsp, const std::vector<uint8_t>& image) {
    for (size_t i = 0; i < image.size(); ++i) scsp.write_ram(uint32_t(i), 1, image[i]);
}

// Timer B (prescaler 0, contagem 0xFE: estoura em 2 amostras) -> SCILV nível 2, SCIEB habilitado.
void arm_timer_b(Scsp& scsp) {
    scsp.write_reg(Scsp::kTimerB, 2, 0x00FE);
    scsp.write_reg(Scsp::kScilv1, 2, 0x0080);
    scsp.write_reg(Scsp::kScieb, 2, 0x0080);
}
}  // namespace

TEST_CASE("SCSP: a RAM de som é espelhada e o 68000 a vê em 0 enquanto os registradores aparecem em 0x100000") {
    Scsp scsp;
    scsp.write_ram(0x10, 4, 0x11223344);
    CHECK(scsp.read_ram(0x80010, 4) == 0x11223344u);               // espelho de 512 KiB
    CHECK(scsp.read16(0x10) == 0x1122u);
    CHECK(scsp.read8(0x13) == 0x44u);

    scsp.write16(0x20, 0xBEEF);
    CHECK(scsp.read_ram(0x20, 2) == 0xBEEFu);

    scsp.write_reg(0x400, 2, 0x0F00);                              // MVOL: armazenamento simples
    CHECK(scsp.read16(0x100400) == 0x0F00u);
    scsp.write8(0x100401, 0x07);
    CHECK(scsp.read_reg(0x400, 2) == 0x0F07u);
    CHECK(scsp.read_reg(0x400, 4) == 0x0F070000u);                 // acesso de 32 bits big-endian
}

TEST_CASE("SCSP: o Timer A conta a cada amostra e o estouro levanta SCIPD e MCIPD") {
    Scsp scsp;
    scsp.write_reg(Scsp::kTimerA, 2, 0x00FF);
    CHECK((scsp.read_reg(Scsp::kScipd, 2) & Scsp::kTimerABit) == 0);
    advance_samples(scsp, 1);
    CHECK(scsp.read_reg(Scsp::kTimerA, 2) == 0x0000u);
    CHECK((scsp.read_reg(Scsp::kScipd, 2) & Scsp::kTimerABit) != 0);
    CHECK((scsp.read_reg(Scsp::kMcipd, 2) & Scsp::kTimerABit) != 0);

    scsp.write_reg(Scsp::kScire, 2, Scsp::kTimerABit);             // escrever 1 em SCIRE limpa a pendência
    CHECK((scsp.read_reg(Scsp::kScipd, 2) & Scsp::kTimerABit) == 0);
    CHECK((scsp.read_reg(Scsp::kMcipd, 2) & Scsp::kTimerABit) != 0);   // o outro lado é independente
}

TEST_CASE("SCSP: o divisor do timer (TACTL) conta uma vez a cada 2^TACTL amostras") {
    Scsp scsp;
    scsp.write_reg(Scsp::kTimerC, 2, 0x02FE);                      // TACTL = 2: 4 amostras por contagem
    advance_samples(scsp, 3);
    CHECK((scsp.read_reg(Scsp::kTimerC, 2) & 0xFF) == 0xFE);
    advance_samples(scsp, 1);
    CHECK((scsp.read_reg(Scsp::kTimerC, 2) & 0xFF) == 0xFF);       // o primeiro tique cai na amostra 4
    advance_samples(scsp, 4);
    CHECK((scsp.read_reg(Scsp::kScipd, 2) & Scsp::kTimerCBit) != 0);
}

TEST_CASE("SCSP: SCIPD só aceita o bit 5 (interrupção manual); SCIRE e MCIRE limpam") {
    Scsp scsp;
    scsp.write_reg(Scsp::kScipd, 2, 0x0040);                       // bit de timer não é gravável
    CHECK(scsp.read_reg(Scsp::kScipd, 2) == 0u);
    scsp.write_reg(Scsp::kScipd, 2, Scsp::kManual);
    CHECK(scsp.read_reg(Scsp::kScipd, 2) == Scsp::kManual);
    scsp.write_reg(Scsp::kScire, 2, Scsp::kManual);
    CHECK(scsp.read_reg(Scsp::kScipd, 2) == 0u);

    scsp.write_reg(Scsp::kMcipd, 2, Scsp::kManual);
    CHECK(scsp.read_reg(Scsp::kMcipd, 2) == Scsp::kManual);
    scsp.write_reg(Scsp::kMcire, 1, 0x00);                         // byte alto: não afeta o bit 5
    CHECK(scsp.read_reg(Scsp::kMcipd, 2) == Scsp::kManual);
    scsp.write_reg(Scsp::kMcire + 1, 1, Scsp::kManual);
    CHECK(scsp.read_reg(Scsp::kMcipd, 2) == 0u);
}

TEST_CASE("SCSP: a interrupção para o SH-2 só sai com MCIEB habilitado e uma vez por pendência nova") {
    Scsp scsp;
    int raised = 0;
    scsp.on_main_interrupt = [&] { ++raised; };
    scsp.write_reg(Scsp::kMcipd, 2, Scsp::kManual);
    CHECK(raised == 0);                                            // pendente, mas desabilitada
    scsp.write_reg(Scsp::kMcieb, 2, Scsp::kManual);
    CHECK(raised == 1);                                            // habilitar uma pendência existente dispara
    scsp.write_reg(Scsp::kMcipd, 2, Scsp::kManual);
    CHECK(raised == 1);
    scsp.write_reg(Scsp::kMcire, 2, Scsp::kManual);
    scsp.write_reg(Scsp::kMcipd, 2, Scsp::kManual);
    CHECK(raised == 2);
}

TEST_CASE("SCSP: o 68000 só roda depois de ligado, reseta pelos vetores e o timer o interrompe no nível de SCILV") {
    Scsp scsp;
    load(scsp, interrupt_program());
    arm_timer_b(scsp);

    advance_samples(scsp, 10);
    CHECK(scsp.read_ram(0x800, 2) == 0u);                          // desligado: nada executa

    scsp.power(true);
    CHECK(scsp.cpu().pc == 0x1000u);
    CHECK(scsp.cpu().a[7] == 0xA000u);
    advance_samples(scsp, 10);
    CHECK(scsp.read_ram(0x800, 2) == 0x00AAu);                     // o tratador do nível 2 rodou
}

TEST_CASE("SCSP: o endereço de leitura do slot (CA) acompanha a reprodução e para no fim sem laço") {
    Scsp scsp;
    scsp.write_reg(0x06, 2, 0x2000);                               // LEA
    scsp.write_reg(0x00, 2, 0x1800);                               // KYONEX + KYONB, sem laço, altura 0 = 1 amostra por amostra
    advance_samples(scsp, 4096);
    CHECK(((scsp.read_reg(Scsp::kMonitor, 2) >> 7) & 0xF) == 1u);
    advance_samples(scsp, 6000);                                   // passa do fim: o slot para
    CHECK(((scsp.read_reg(Scsp::kMonitor, 2) >> 7) & 0xF) == 2u);
}

namespace {
std::unique_ptr<saturn::Machine> make_machine(const std::string& tag) {
    testsupport::SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa");
    auto sectors = iso.sectors();
    sectors[0][0xE2] = 0x18;
    sectors[0][0x100] = 0xAF; sectors[0][0x101] = 0xFE;
    sectors[0][0x102] = 0x00; sectors[0][0x103] = 0x09;
    auto machine = std::make_unique<saturn::Machine>(std::make_unique<saturnfs::DiscImage>(
        saturnfs::DiscImage::open(testsupport::write_single_track_disc(testsupport::make_temp_dir(tag), sectors))));
    machine->boot();
    return machine;
}

void smpc_command(saturn::Machine& machine, uint8_t command) {
    machine.bus().write8(kSmpc + kSmpcSf, 1);
    machine.bus().write8(kSmpc + kSmpcComreg, command);
}

void write_image(saturn::Machine& machine, const std::vector<uint8_t>& image) {
    for (size_t i = 0; i < image.size(); ++i) machine.bus().write8(kSh2Ram + uint32_t(i), image[i]);
}
}  // namespace

TEST_CASE("Machine: SNDON liga o 68000, que executa o programa deixado pelo SH-2 na RAM de som; SNDOFF para") {
    auto machine = make_machine("scsp-sndon");
    std::vector<uint8_t> image(0x1100, 0);
    put_words(image, 0, {0x0000, 0xA000, 0x0000, 0x1000});
    put_words(image, 0x1000, {0x33FC, 0x1234, 0x0000, 0x0800, 0x60FE});    // move.w #$1234,$800.l ; bra.s .
    write_image(*machine, image);

    machine->run_cycles(5000);
    CHECK(machine->bus().read16(kSh2Ram + 0x800) == 0u);           // ainda desligado
    smpc_command(*machine, kSndOn);
    machine->run_cycles(5000);
    CHECK(machine->scsp().running());
    CHECK(machine->bus().read16(kSh2Ram + 0x800) == 0x1234u);      // o SH-2 lê o que o 68000 escreveu

    smpc_command(*machine, kSndOff);
    machine->bus().write16(kSh2Ram + 0x800, 0);
    machine->run_cycles(5000);
    CHECK_FALSE(machine->scsp().running());
    CHECK(machine->bus().read16(kSh2Ram + 0x800) == 0u);
}

TEST_CASE("Machine: o timer do SCSP interrompe o 68000, configurado pelo SH-2 pelos registradores em 0x25B00000") {
    auto machine = make_machine("scsp-timer");
    write_image(*machine, interrupt_program());
    machine->bus().write16(kSh2Registers + Scsp::kTimerB, 0x00FE);
    machine->bus().write16(kSh2Registers + Scsp::kScilv1, 0x0080);
    machine->bus().write16(kSh2Registers + Scsp::kScieb, 0x0080);
    smpc_command(*machine, kSndOn);
    machine->run_cycles(20000);
    CHECK(machine->bus().read16(kSh2Ram + 0x800) == 0x00AAu);
    CHECK((machine->bus().read16(kSh2Registers + Scsp::kScipd) & Scsp::kTimerBBit) != 0);
}

// ---- Síntese de áudio ----
namespace {
constexpr uint16_t kKeyOnNow = 0x1800;   // KYONEX + KYONB

// Slot 0: PCM 16 bits em SA=0x1000, AR máximo, TL 0, DISDL 7, pan central; sem MVOL escrito aqui (o teste liga).
void setup_slot(Scsp& scsp, int slot, uint32_t sa, int loop_control, uint16_t lsa, uint16_t lea) {
    const uint32_t b = slot * 0x20;
    scsp.write_reg(0x400, 2, 0x000F);                                   // MVOL máximo
    scsp.write_reg(b + 0x00, 2, (loop_control << 5) | (sa >> 16));
    scsp.write_reg(b + 0x02, 2, sa & 0xFFFF);
    scsp.write_reg(b + 0x04, 2, lsa);
    scsp.write_reg(b + 0x06, 2, lea);
    scsp.write_reg(b + 0x08, 2, 0x001F);                                // AR = 31
    scsp.write_reg(b + 0x0A, 2, 0x001F);                                // RR = 31
    scsp.write_reg(b + 0x0C, 2, 0x0000);                                // TL = 0
    scsp.write_reg(b + 0x16, 2, 0xE000);                                // DISDL 7, pan central
}
void put_pcm16(Scsp& scsp, uint32_t at, const std::vector<int16_t>& v) {
    for (size_t i = 0; i < v.size(); ++i) scsp.write_ram(at + uint32_t(2 * i), 2, uint16_t(v[i]));
}
void key_on(Scsp& scsp, int slot) { scsp.write_reg(slot * 0x20, 2, scsp.read_reg(slot * 0x20, 2) | kKeyOnNow); }
void key_off(Scsp& scsp, int slot) { scsp.write_reg(slot * 0x20, 2, (scsp.read_reg(slot * 0x20, 2) & ~0x800u) | 0x1000); }
std::vector<int16_t> drain_all(Scsp& scsp) {
    std::vector<int16_t> out(100000);
    out.resize(scsp.drain_audio(out));
    return out;
}
}  // namespace

TEST_CASE("SCSP áudio: drain_audio sem amostras devolve 0 e desligado não gera nada") {
    Scsp scsp;
    std::vector<int16_t> out(64);
    CHECK(scsp.drain_audio(out) == 0);
    setup_slot(scsp, 0, 0x1000, 1, 0, 100);
    put_pcm16(scsp, 0x1000, std::vector<int16_t>(100, 0x1000));
    key_on(scsp, 0);
    advance_samples(scsp, 50);
    CHECK(scsp.drain_audio(out) == 0);   // padrão: desligado
    scsp.set_audio_enabled(true);
    scsp.set_audio_enabled(false);
    advance_samples(scsp, 50);
    CHECK(scsp.drain_audio(out) == 0);
}

TEST_CASE("SCSP áudio: PCM constante com AR máximo, TL 0 e pan central sai com o valor esperado nos dois canais") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 1, 0, 100);
    put_pcm16(scsp, 0x1000, std::vector<int16_t>(100, 0x1000));
    key_on(scsp, 0);
    advance_samples(scsp, 20);
    const auto out = drain_all(scsp);
    REQUIRE(out.size() == 40);
    for (size_t i = 10; i < out.size(); ++i) CHECK(out[i] == 0x1000);
}

TEST_CASE("SCSP áudio: KEY OFF com RR máximo leva a saída a zero; atenuação TL e DISDL reduzem o nível") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 1, 0, 100);
    put_pcm16(scsp, 0x1000, std::vector<int16_t>(100, 0x2000));
    key_on(scsp, 0);
    advance_samples(scsp, 10);
    key_off(scsp, 0);
    advance_samples(scsp, 300);
    drain_all(scsp);
    advance_samples(scsp, 10);
    for (int16_t v : drain_all(scsp)) CHECK(v == 0);

    Scsp half;
    half.set_audio_enabled(true);
    setup_slot(half, 0, 0x1000, 1, 0, 100);
    half.write_reg(0x16, 2, 0xC000);   // DISDL 6 = -6 dB
    put_pcm16(half, 0x1000, std::vector<int16_t>(100, 0x2000));
    key_on(half, 0);
    advance_samples(half, 10);
    const auto out = drain_all(half);
    CHECK(out.back() == 0x1000);
}

TEST_CASE("SCSP áudio: laço normal repete e sem laço o som termina") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 1, 0, 4);
    put_pcm16(scsp, 0x1000, {1000, 2000, 3000, 4000, 9999, 9999});
    key_on(scsp, 0);
    advance_samples(scsp, 12);
    const auto out = drain_all(scsp);
    REQUIRE(out.size() == 24);
    for (size_t n = 0; n < 12; ++n) CHECK(out[2 * n] == 1000 * int(((n + 1) % 4) + 1));   // a posição avança antes da primeira amostra

    Scsp once;
    once.set_audio_enabled(true);
    setup_slot(once, 0, 0x1000, 0, 0, 4);
    put_pcm16(once, 0x1000, {1000, 2000, 3000, 4000, 9999, 9999});
    key_on(once, 0);
    advance_samples(once, 12);
    const auto single = drain_all(once);
    CHECK(single[2 * 11] == 0);
    CHECK(single[2 * 1] == 3000);
}

TEST_CASE("SCSP áudio: subir uma oitava (OCT = 1) dobra a taxa de leitura") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 0, 0, 100);
    std::vector<int16_t> ramp(100);
    for (int i = 0; i < 100; ++i) ramp[i] = int16_t(i * 100);
    put_pcm16(scsp, 0x1000, ramp);
    scsp.write_reg(0x10, 2, 1 << 11);   // OCT = 1, FNS = 0
    key_on(scsp, 0);
    advance_samples(scsp, 11);
    const auto out = drain_all(scsp);
    CHECK(out[2 * 10] == 2200);         // posição 22 (2 por amostra)
    CHECK(out[2 * 5] == 1200);
}

TEST_CASE("SCSP áudio: a interpolação linear entre amostras vizinhas de saltos grandes não estoura 32 bits (sem estalo isolado)") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 0, 0, 100);
    std::vector<int16_t> square(100);
    for (int i = 0; i < 100; ++i) square[i] = i % 2 ? int16_t(-30000) : int16_t(30000);
    put_pcm16(scsp, 0x1000, square);
    scsp.write_reg(0x10, 2, 0xD << 11);   // OCT = -3: 1/8 de amostra por quadro, varrendo todas as frações entre +30000 e -30000
    key_on(scsp, 0);
    advance_samples(scsp, 40);
    const auto out = drain_all(scsp);
    REQUIRE(out.size() == 80);
    for (size_t i = 2; i < 40; ++i) {
        CHECK(std::abs(out[2 * i]) <= 30000);                  // sempre entre as duas amostras vizinhas
        CHECK(std::abs(out[2 * i] - out[2 * (i - 1)]) <= 8000);   // passo de 60000/8 por quadro, sem salto
    }
}

TEST_CASE("SCSP áudio: DIPAN com o bit 4 desligado zera o canal esquerdo e ligado o direito (como o Mednafen); PCM 8 bits") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 1, 0, 100);
    setup_slot(scsp, 1, 0x2000, 1, 0, 100);
    put_pcm16(scsp, 0x1000, std::vector<int16_t>(100, 0x1000));
    for (int i = 0; i < 100; ++i) scsp.write_ram(0x2000 + i, 1, 0x10);
    scsp.write_reg(0x16, 2, 0xE000 | 0x1F00);   // DIPAN 0x1F: o canal direito é atenuado por completo (tudo à esquerda)
    scsp.write_reg(0x20, 2, scsp.read_reg(0x20, 2) | 0x10);   // slot 1 em PCM 8 bits
    scsp.write_reg(0x20 + 0x16, 2, 0xE000 | 0x0F00);          // DIPAN 0x0F: o esquerdo é atenuado por completo (tudo à direita)
    key_on(scsp, 0);
    key_on(scsp, 1);   // KYONEX uma vez já aplica os dois; o segundo é inofensivo
    advance_samples(scsp, 10);
    const auto out = drain_all(scsp);
    CHECK(out[2 * 9] == 0x1000);       // esquerda: só o slot 0
    CHECK(out[2 * 9 + 1] == 0x1000);   // direita: só o slot 1 (0x10 << 8)
}

TEST_CASE("SCSP áudio: a mixagem satura em 16 bits e o anel interno não passa do seu tamanho") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_slot(scsp, 0, 0x1000, 1, 0, 100);
    setup_slot(scsp, 1, 0x1000, 1, 0, 100);
    put_pcm16(scsp, 0x1000, std::vector<int16_t>(100, 30000));
    key_on(scsp, 0);
    key_on(scsp, 1);
    advance_samples(scsp, 20);
    const auto out = drain_all(scsp);
    CHECK(out.back() == 32767);

    Scsp longrun;
    longrun.set_audio_enabled(true);
    advance_samples(longrun, int(Scsp::kAudioBufferFrames) + 5000);
    std::vector<int16_t> big(Scsp::kAudioBufferFrames * 4);
    CHECK(longrun.drain_audio(big) == Scsp::kAudioBufferFrames * 2);
}


// ---- DSP de efeitos, entradas externas e registradores de trabalho ----
namespace {
// Instrução do DSP (64 bits) montada pelos campos que os testes usam.
struct Instr {
    uint64_t v = 0;
    Instr& at(int bit, uint64_t value) { v |= value << bit; return *this; }
    Instr& nofl() { return at(8, 1); }
    Instr& shft0() { return at(20, 1); }   // deslocador x2
    Instr& cra(unsigned n) { return at(9, n); }
    Instr& zero() { return at(17, 1); }
    Instr& ewt(unsigned ewa) { return at(24, ewa).at(28, 1); }
    Instr& mrt() { return at(29, 1); }
    Instr& mwt() { return at(30, 1); }
    Instr& table() { return at(31, 1); }
    Instr& iwt(unsigned iwa) { return at(32, iwa).at(37, 1); }
    Instr& ira(unsigned n) { return at(38, n); }
    Instr& ysel_coef() { return at(45, 1); }
    Instr& xsel_inputs() { return at(47, 1); }
};

void put_instr(Scsp& scsp, int step, const Instr& i) {
    for (int b = 0; b < 8; ++b) scsp.write_reg(Scsp::kDspProgram + 8 * step + b, 1, (i.v >> (8 * (7 - b))) & 0xFF);
}
// Coeficiente 0 = 0,5 (o COEF é de 13 bits com sinal: 1,0 não existe); o deslocador x2 do passo seguinte devolve o valor inteiro.
constexpr uint16_t kHalfCoefficient = 0x0800;
// Passa a entrada `ira` pelo coeficiente 0 ao SFT_REG; o passo seguinte a desloca (x2) e a leva ao EFREG `ewa`.
void put_passthrough(Scsp& scsp, int step, unsigned ira, unsigned ewa) {
    put_instr(scsp, step, Instr().ira(ira).xsel_inputs().ysel_coef().zero());
    put_instr(scsp, step + 1, Instr().shft0().ewt(ewa));
}
// Início comum dos programas com anel: SFT_REG = entrada MIXS[0] * 0,5.
void put_ring_input(Scsp& scsp) { put_instr(scsp, 0, Instr().ira(0x20).xsel_inputs().ysel_coef().zero()); }
// Slot 0 em laço com `value` constante, enviado ao DSP (IMXL 7, ISEL 0); saída direta desligada; EFSDL 7 e pan central em 0.
void setup_dsp_slot(Scsp& scsp, int16_t value, unsigned efsdl) {
    setup_slot(scsp, 0, 0x1000, 1, 0, 100);
    put_pcm16(scsp, 0x1000, std::vector<int16_t>(100, value));
    scsp.write_reg(0x14, 2, 0x0007);
    scsp.write_reg(0x16, 2, uint16_t(efsdl << 5));
    scsp.write_reg(Scsp::kDspCoef, 2, kHalfCoefficient << 3);
}
}  // namespace

TEST_CASE("SCSP DSP: um programa de passagem leva o slot (IMXL) ao EFREG e à saída por EFSDL/EFPAN") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_dsp_slot(scsp, 0x1000, 7);
    put_passthrough(scsp, 0, 0x20, 0);   // MIXS[0] -> EFREG[0]
    key_on(scsp, 0);
    advance_samples(scsp, 20);
    const auto out = drain_all(scsp);
    REQUIRE(out.size() == 40);
    CHECK(scsp.read_reg(Scsp::kDspEfreg, 2) == 0x1000u);
    for (size_t i = 10; i < out.size(); ++i) CHECK(out[i] == 0x1000);   // pan central: os dois canais

    // EFSDL 6 = metade do nível; EFPAN 0x1F: só o canal esquerdo.
    Scsp half;
    half.set_audio_enabled(true);
    setup_dsp_slot(half, 0x1000, 6);
    half.write_reg(0x16, 2, (6 << 5) | 0x1F);
    put_passthrough(half, 0, 0x20, 0);
    key_on(half, 0);
    advance_samples(half, 20);
    const auto halved = drain_all(half);
    CHECK(halved[2 * 15] == 0x800);
    CHECK(halved[2 * 15 + 1] == 0);
}

TEST_CASE("SCSP DSP: sem programa (ou sem EFSDL) o envio IMXL não vaza para a saída") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_dsp_slot(scsp, 0x1000, 7);
    key_on(scsp, 0);
    advance_samples(scsp, 20);
    for (int16_t v : drain_all(scsp)) CHECK(v == 0);   // MPRO vazio: EFREG fica em 0

    Scsp mute;
    mute.set_audio_enabled(true);
    setup_dsp_slot(mute, 0x1000, 0);
    put_passthrough(mute, 0, 0x20, 0);
    key_on(mute, 0);
    advance_samples(mute, 20);
    for (int16_t v : drain_all(mute)) CHECK(v == 0);   // EFSDL = 0 silencia o efeito
    CHECK(mute.read_reg(Scsp::kDspEfreg, 2) == 0x1000u);
}

TEST_CASE("SCSP DSP: anel de atraso na RAM de som (escrita sem conversão, leitura e MEMS)") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    setup_dsp_slot(scsp, 0x1234, 7);
    scsp.write_reg(Scsp::kRingBuffer, 2, 0x0001);   // RBP = 1 (8 KiB de palavras), RBL = 0
    scsp.write_reg(Scsp::kDspMadrs, 2, 0x0010);     // MADRS[0] = 0x10 palavras
    put_ring_input(scsp);
    put_instr(scsp, 1, Instr().shft0().mwt().nofl().table());                         // grava o deslocador
    put_instr(scsp, 3, Instr().mrt().nofl().table());                         // lê de volta
    put_instr(scsp, 5, Instr().iwt(0));                                       // MEMS[0] = valor lido
    put_passthrough(scsp, 6, 0x00, 0);                                        // MEMS[0] -> EFREG[0]
    key_on(scsp, 0);
    advance_samples(scsp, 20);
    CHECK(scsp.read_ram((0x1000 + 0x10) * 2, 2) == 0x1234u);   // palavra (RBP << 12) + MADRS
    const auto out = drain_all(scsp);
    CHECK(out[2 * 15] == 0x1234);
    CHECK(out[2 * 15 + 1] == 0x1234);
}

TEST_CASE("SCSP DSP: escrita e leitura em ponto flutuante de 16 bits voltam o valor com a precisão da mantissa") {
    for (const int16_t value : {int16_t(0x1000), int16_t(-0x1000), int16_t(0x1234), int16_t(-0x1234), int16_t(0x7F00), int16_t(5)}) {
        Scsp scsp;
        scsp.set_audio_enabled(true);
        setup_dsp_slot(scsp, value, 7);
        scsp.write_reg(Scsp::kRingBuffer, 2, 0x0001);
        put_ring_input(scsp);
        put_instr(scsp, 1, Instr().shft0().mwt().table());
        put_instr(scsp, 3, Instr().mrt().table());
        put_instr(scsp, 5, Instr().iwt(0));
        put_passthrough(scsp, 6, 0x00, 0);
        key_on(scsp, 0);
        advance_samples(scsp, 20);
        const int16_t back = int16_t(scsp.read_reg(Scsp::kDspEfreg, 2));
        INFO("valor " << value << " voltou " << back);
        CHECK(std::abs(back - value) <= std::max(1, std::abs(value) >> 10));   // 11 bits de mantissa
    }
}

TEST_CASE("SCSP DSP: as entradas externas (EXTS) vão ao DSP e às saídas de efeito dos slots 16 e 17") {
    Scsp scsp;
    scsp.set_audio_enabled(true);
    scsp.write_reg(0x400, 2, 0x000F);   // MVOL máximo
    scsp.write_reg(Scsp::kDspCoef, 2, kHalfCoefficient << 3);
    put_passthrough(scsp, 0, 0x30, 0);   // EXTS[0] -> EFREG[0]
    put_passthrough(scsp, 2, 0x31, 1);   // EXTS[1] -> EFREG[1]
    scsp.write_reg(16 * 0x20 + 0x16, 2, (7 << 5) | 0x1F);   // slot 16 (EXTS esquerdo): só o canal esquerdo
    scsp.write_reg(17 * 0x20 + 0x16, 2, (6 << 5) | 0x0F);   // slot 17 (EXTS direito): metade, só o canal direito
    scsp.set_external_input(1000, -2000);
    advance_samples(scsp, 10);
    CHECK(int16_t(scsp.read_reg(Scsp::kDspEfreg, 2)) == 1000);
    CHECK(int16_t(scsp.read_reg(Scsp::kDspEfreg + 2, 2)) == -2000);
    CHECK(scsp.read_reg(Scsp::kDspExts, 4) == 0x03E8F830u);   // EXTS é legível
    const auto out = drain_all(scsp);
    CHECK(out[2 * 5] == 1000);
    CHECK(out[2 * 5 + 1] == -1000);
    scsp.write_reg(Scsp::kDspExts, 2, 0x1234);   // só leitura
    CHECK(scsp.read_reg(Scsp::kDspExts, 2) == 0x03E8u);
}

TEST_CASE("SCSP DSP: TEMP, MEMS e MIXS guardam 24, 24 e 20 bits com as duas palavras de cada valor") {
    Scsp scsp;
    scsp.write_reg(Scsp::kDspTemp, 4, 0x00EFABCD);
    CHECK(scsp.read_reg(Scsp::kDspTemp, 4) == 0x00EFABCDu);
    scsp.write_reg(Scsp::kDspTemp, 4, 0xFFFFFFFF);   // os bits que não existem somem
    CHECK(scsp.read_reg(Scsp::kDspTemp, 4) == 0x00FFFFFFu);
    scsp.write_reg(Scsp::kDspMems + 4, 4, 0x0012A5A5);
    CHECK(scsp.read_reg(Scsp::kDspMems + 4, 4) == 0x0012A5A5u);
    CHECK(scsp.read_reg(Scsp::kDspMems, 4) == 0u);
    scsp.write_reg(Scsp::kDspMixs, 4, 0xFFFFFFFF);
    CHECK(scsp.read_reg(Scsp::kDspMixs, 4) == 0x000FFFFFu);
}

TEST_CASE("SCSP DSP: o estado do DSP entra no save state (saída idêntica depois de recarregar)") {
    const auto setup = [](Scsp& scsp) {
        scsp.set_audio_enabled(true);
        setup_dsp_slot(scsp, 0x0800, 7);
        scsp.write_reg(Scsp::kRingBuffer, 2, 0x0001);
        put_ring_input(scsp);
        put_instr(scsp, 1, Instr().shft0().mwt().table());
        put_instr(scsp, 3, Instr().mrt().table());
        put_instr(scsp, 5, Instr().iwt(0));
        put_passthrough(scsp, 6, 0x00, 0);
        key_on(scsp, 0);
    };
    Scsp original;
    setup(original);
    advance_samples(original, 300);
    drain_all(original);
    saturn::StateWriter w;
    original.save(w);
    const auto bytes = w.take();
    advance_samples(original, 100);
    const auto expected = drain_all(original);

    Scsp restored;
    restored.set_audio_enabled(true);
    saturn::StateReader r(bytes);
    restored.load(r);
    CHECK(r.remaining() == 0);
    advance_samples(restored, 100);
    CHECK(drain_all(restored) == expected);
    CHECK(expected.back() != 0);
}
