#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <array>
#include <vector>

#include "iso_builder.hpp"
#include "saturn/machine.hpp"

using namespace testsupport;
using saturn::Machine;
using saturn::MachineError;

namespace {
constexpr uint32_t kIpBase = 0x06002000;

// Disco sintético com um "IP": cabeçalho Saturn com tamanho 0x1800 em +0xE0 e, em +0x100, um laço `bra .` (com nop no slot).
std::unique_ptr<saturnfs::DiscImage> make_disc(const std::string& tag) {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa");
    auto sectors = iso.sectors();
    sectors[0][0xE2] = 0x18;                                  // IP size = 0x00001800 (big-endian)
    sectors[0][0x100] = 0xAF; sectors[0][0x101] = 0xFE;       // bra -2 (laço infinito)
    sectors[0][0x102] = 0x00; sectors[0][0x103] = 0x09;       // nop (slot)
    sectors[1][0] = 0x12; sectors[1][1] = 0x34;               // marca do 2º setor do IP
    return std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(write_single_track_disc(make_temp_dir(tag), sectors)));
}

// Chama um serviço do BIOS como o jogo faria: lê o ponteiro no slot e salta para ele com PR = retorno.
void call_slot(Machine& m, uint32_t slot, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
    constexpr uint32_t kReturn = 0x06003000;
    m.bus().write16(kReturn, 0x0009);
    auto& s = m.master().state();
    s.pc = m.bus().read32(slot);
    s.pr = kReturn;
    s.r[4] = r4; s.r[5] = r5; s.r[6] = r6;
    m.step_master();
}
// Comando do SMPC como o jogo envia: SF = 1 e depois COMREG.
void smpc_command(Machine& m, uint8_t command) {
    m.smpc().write(0x63, 1, 1);
    m.smpc().write(0x1F, 1, command);
}
constexpr uint8_t kSshOn = 0x02, kSshOff = 0x03;

void step(Machine& m, int instructions) {
    for (int i = 0; i < instructions; ++i) m.step_master();
}

// Laço `bra .` (com nop no slot) em `entry`.
void write_spin_loop(Machine& m, uint32_t entry) {
    m.bus().write16(entry, 0xAFFE);
    m.bus().write16(entry + 2, 0x0009);
}
}  // namespace

TEST_CASE("boot carrega o IP em 0x06002000 e começa a executar em 0x06002100") {
    Machine m(make_disc("m1"));
    m.boot();

    CHECK(m.master().state().pc == 0x06002100u);
    CHECK(m.bus().read16(kIpBase + 0x100) == 0xAFFE);
    CHECK(m.bus().read16(kIpBase + 0x800) == 0x1234);     // 2º setor do IP
    CHECK(m.master().state().vbr == 0x06000000u);
    CHECK(m.master().state().sr == 0xF0u);                // interrupções mascaradas até o IP liberar
}

TEST_CASE("BIOS: o serviço de vetor (slot 0x06000300) registra o tratador na tabela da BIOS") {
    Machine m(make_disc("m2"));
    m.boot();

    call_slot(m, 0x06000300, 0x40, 0x060022E4);

    CHECK(m.bus().read32(0x06000900 + 0x40 * 4) == 0x060022E4u);  // tabela do SCU no layout do BIOS
    CHECK(m.master().state().pc == 0x06003000u);          // retornou ao chamador (PR)
}

TEST_CASE("BIOS: o slot 0x06000304 devolve o tratador do SCU registrado pelo slot 0x06000300") {
    Machine m(make_disc("m2-get"));
    m.boot();
    call_slot(m, 0x06000300, 0x41, 0x06002400);

    call_slot(m, 0x06000304, 0x41);

    CHECK(m.master().state().r[0] == 0x06002400u);
}

TEST_CASE("BIOS: o slot 0x06000310 grava o tratador do SH-2 na tabela do VBR de quem chama e o 0x06000314 o devolve") {
    Machine m(make_disc("m2-sint"));
    m.boot();
    const uint32_t original = m.bus().read32(0x06000000 + 0x40 * 4);

    call_slot(m, 0x06000310, 0x94, 0x060139C4);            // vetor 0x94 = ponto de entrada do escravo
    CHECK(m.bus().read32(0x06000250) == 0x060139C4u);
    call_slot(m, 0x06000314, 0x94);
    CHECK(m.master().state().r[0] == 0x060139C4u);

    call_slot(m, 0x06000310, 0x40, 0x06002500);
    CHECK(m.bus().read32(0x06000000 + 0x40 * 4) == 0x06002500u);
    call_slot(m, 0x06000310, 0x40, 0);                     // r5 = 0 restaura o padrão do BIOS
    CHECK(m.bus().read32(0x06000000 + 0x40 * 4) == original);
    CHECK(m.master().state().pc == 0x06003000u);
}

TEST_CASE("sem BIOS real, SSHOFF + SYS_SETSINT(0x94) + SSHON parte o escravo no novo ponto de entrada") {
    Machine m(make_disc("m7-sint"));
    m.boot();
    constexpr uint32_t kOld = 0x06003100, kNew = 0x06003200;
    write_spin_loop(m, kOld);
    write_spin_loop(m, kNew);
    m.bus().write32(saturn::BiosHle::kSlaveEntryPointer, kOld);
    smpc_command(m, kSshOn);
    step(m, 4);
    REQUIRE(m.slave().state().pc <= kOld + 2);

    smpc_command(m, kSshOff);
    call_slot(m, 0x06000310, 0x94, kNew);
    m.master().state().pc = kOld;                           // mestre de volta a um laço
    smpc_command(m, kSshOn);
    step(m, 4);

    CHECK(m.slave().state().pc >= kNew);
    CHECK(m.slave().state().pc <= kNew + 2);
}

TEST_CASE("BIOS: o serviço de máscara do SCU (slot 0x06000344) faz (IMS & r4) | r5") {
    Machine m(make_disc("m3"));
    m.boot();
    CHECK(m.scu().read(0xA0, 4) == 0xFFFFFFFFu);

    call_slot(m, 0x06000344, 0xFFFFFFFE, 0);               // libera o bit 0 (VBlank-IN)

    CHECK(m.scu().read(0xA0, 4) == 0xFFFFFFFEu);
}

TEST_CASE("BIOS: serviço não implementado para a execução com o endereço do slot na mensagem") {
    Machine m(make_disc("m4"));
    m.boot();

    CHECK_THROWS_WITH(call_slot(m, 0x06000320), Catch::Matchers::ContainsSubstring("0x06000320"));
}

TEST_CASE("vetor de interrupção sem handler do jogo é atendido pelo BIOS com rte e o programa continua") {
    Machine m(make_disc("m5"));
    m.boot();
    m.scu().write(0xA0, 4, 0);                              // desmascara tudo
    m.master().state().sr = 0;                              // SR.I = 0
    const uint32_t before = m.master().state().pc;

    m.scu().raise(saturn::Scu::Interrupt::VBlankIn);        // vetor 0x40: ainda no handler padrão
    m.step_master();                                        // entra na exceção
    m.step_master();                                        // o gatilho do BIOS executa o rte
    CHECK(m.master().state().pc == before);
}

TEST_CASE("exceção sem tratamento (instrução ilegal) para a execução com vetor e PC") {
    Machine m(make_disc("m6"));
    m.boot();
    m.bus().write16(0x06002100, 0xFFFF);

    m.step_master();                                        // entra no vetor 4 -> gatilho fatal
    CHECK_THROWS_WITH(m.step_master(), Catch::Matchers::ContainsSubstring("vetor 4"));
}

TEST_CASE("observador de passos registra PC antes da execução e pode ser desativado") {
    Machine m(make_disc("m6-history"));
    m.boot();
    m.bus().write16(0x06002100, 0x0009);  // nop
    std::vector<uint32_t> pcs;
    m.on_before_master_step = [&](const sh2::Sh2State& state) { pcs.push_back(state.pc); };
    m.step_master();
    m.step_master();
    REQUIRE(pcs.size() == 2);
    CHECK(pcs[0] == 0x06002100u);
    CHECK(pcs[1] == 0x06002102u);
    m.on_before_master_step = {};
    m.step_master();
    CHECK(pcs.size() == 2);
}

TEST_CASE("o SH-2 escravo só roda depois do SSHON") {
    Machine m(make_disc("m7"));
    m.boot();
    CHECK_FALSE(m.slave_running());

    smpc_command(m, kSshOn);
    CHECK(m.slave_running());
    smpc_command(m, kSshOff);
    CHECK_FALSE(m.slave_running());
}

TEST_CASE("sem BIOS real, o SSHON parte o escravo no ponto de entrada registrado em 0x06000250 e a escrita em SINIT levanta o ICF dele") {
    Machine m(make_disc("m7-hle"));
    m.boot();
    constexpr uint32_t kEntry = 0x06003000;
    m.bus().write32(saturn::BiosHle::kSlaveEntryPointer, kEntry);
    write_spin_loop(m, kEntry);

    smpc_command(m, kSshOn);
    step(m, 4);

    CHECK(m.slave().state().pc >= kEntry);
    CHECK(m.slave().state().pc <= kEntry + 2);
    CHECK(m.slave().state().r[15] == 0x06001000u);          // pilha padrão do escravo (IP +0xEC = 0)
    CHECK((m.slave().onchip().read(0x11, 1) & 0x80) == 0);
    m.bus().write16(0x01000000, 0xFFFF);                    // SINIT
    CHECK((m.slave().onchip().read(0x11, 1) & 0x80) == 0x80);
    CHECK((m.master().onchip().read(0x11, 1) & 0x80) == 0);
    m.bus().write16(0x01800000, 0xFFFF);                    // MINIT
    CHECK((m.master().onchip().read(0x11, 1) & 0x80) == 0x80);
}

TEST_CASE("reiniciar o escravo (SSHOFF/SSHON) zera o ICF acumulado por SINIT antes da partida") {
    Machine m(make_disc("m7-icf"));
    m.boot();
    constexpr uint32_t kEntry = 0x06003000;
    m.bus().write32(saturn::BiosHle::kSlaveEntryPointer, kEntry);
    write_spin_loop(m, kEntry);
    smpc_command(m, kSshOn);
    step(m, 4);
    m.bus().write16(0x01000000, 0xFFFF);                    // SINIT
    CHECK((m.slave().onchip().read(0x11, 1) & 0x80) == 0x80);

    smpc_command(m, kSshOff);
    smpc_command(m, kSshOn);
    step(m, 4);
    CHECK((m.slave().onchip().read(0x11, 1) & 0x80) == 0);
}

TEST_CASE("sem BIOS real, uma exceção no escravo o para e fica registrada, sem derrubar o mestre") {
    Machine m(make_disc("m7-fault"));
    m.boot();
    constexpr uint32_t kEntry = 0x06003000;
    m.bus().write32(saturn::BiosHle::kSlaveEntryPointer, kEntry);
    m.bus().write16(kEntry, 0xE101);                        // mov #1,r1
    m.bus().write16(kEntry + 2, 0x6112);                    // mov.l @r1,r1: leitura desalinhada -> erro de endereço
    smpc_command(m, kSshOn);

    step(m, 16);

    CHECK_FALSE(m.slave_running());
    CHECK_FALSE(m.slave_fault().empty());
    smpc_command(m, kSshOn);
    CHECK(m.slave_running());
    CHECK(m.slave_fault().empty());
}

TEST_CASE("run_cycles avança o vídeo: um quadro entrega o VBlank-IN ao mestre e frame() == 1") {
    Machine m(make_disc("m8"));
    m.boot();
    m.scu().write(0xA0, 4, 0xFFFFFFFE);                     // só o VBlank-IN
    m.bus().write32(0x06000000 + 0x40 * 4, 0x06002800);     // handler do jogo: rte
    m.bus().write16(0x06002800, 0x002B);                    // rte
    m.bus().write16(0x06002802, 0x0009);                    // nop (slot)
    m.master().state().sr = 0;
    int vblanks = 0;
    m.on_interrupt_taken = [&](int, int vector) { if (vector == 0x40) ++vblanks; };

    m.run_frame();

    CHECK(m.frame() == 1);
    CHECK(vblanks >= 1);
}

TEST_CASE("BIOS: o serviço gráfico 0x5E4 exige uma captura de 4096 bytes da fonte descompactada") {
    Machine m(make_disc("m9"));
    m.boot();

    CHECK_THROWS_WITH(call_slot(m, 0x000005E4), Catch::Matchers::ContainsSubstring("0x000005E4"));
}

TEST_CASE("BIOS 0x5E4: com a fonte opcional, a falta dela só deixa o texto do boot em branco") {
    Machine m(make_disc("m9-optional"));
    m.boot();
    m.set_boot_font_required(false);

    call_slot(m, 0x000005E4);

    CHECK(m.master().state().pc == 0x06003000u);
}

TEST_CASE("BIOS 0x5E4: expande os 4096 bytes da fonte em 16384 bytes de VRAM") {
    Machine m(make_disc("m9-font"));
    m.boot();
    std::vector<uint8_t> font(4096, 0);
    font.front() = 0x1B;  // 00, 01, 10, 11: tabela de 0x5782 da BIOS
    font.back() = 0xFF;
    CHECK_THROWS_AS(m.set_boot_font(std::vector<uint8_t>(4095)), std::invalid_argument);
    m.set_boot_font(font);

    call_slot(m, 0x000005E4);

    CHECK(m.master().state().pc == 0x06003000u);
    CHECK(m.bus().read8(0x06000E20) == 0x1Bu);
    CHECK(m.bus().read8(0x06001E1F) == 0xFFu);
    const std::array<uint8_t, 4> expected{0, 0x0D, 0xD0, 0xDD};
    for (uint32_t i = 0; i < 4; ++i) CHECK(m.bus().read8(0x25E00000 + i) == expected[i]);
    CHECK(m.bus().read8(0x25E00004) == 0);
    for (uint32_t i = 0; i < 4; ++i) CHECK(m.bus().read8(0x25E03FFC + i) == 0xDDu);
}

TEST_CASE("BIOS 0x5E8: desenha 20 e 12 caracteres e a linha complementar com os planos separados") {
    Machine m(make_disc("m9-text"));
    m.boot();
    constexpr uint32_t text = 0x06003200, footer = 0x06003300;
    for (uint32_t i = 0; i < 32; ++i) m.bus().write8(text + i, static_cast<uint8_t>('A' + i));
    m.bus().write8(footer, 'Z');
    m.bus().write8(footer + 1, 0);
    m.bus().write32(0x06000248, 0x0800);  // deslocamento do plano = +0x100

    call_slot(m, 0x000005E8, text, footer);

    CHECK(m.master().state().pc == 0x06003000u);
    CHECK(m.bus().read16(0x25E06988 + 0x100) == 2 * 'A');
    CHECK(m.bus().read16(0x25E06988 + 0x100 + 0x80) == 2 * 'A' + 1);
    CHECK(m.bus().read16(0x25E06988 + 0x100 + 38) == 2 * ('A' + 19));
    CHECK(m.bus().read16(0x25E06988 + 0x100 + 0x2A) == 2 * ('A' + 20));
    CHECK(m.bus().read16(0x25E06988 + 0x100 + 0x2A + 22) == 2 * ('A' + 31));
    CHECK(m.bus().read16(0x25E06B14 + 0x100) == 2 * 'Z');
    CHECK(m.bus().read16(0x25E06B14 + 0x100 + 0x80) == 2 * 'Z' + 1);
    CHECK(m.bus().read16(0x25E06B14 + 0x100 + 2) == 0); // terminador não é desenhado
}

TEST_CASE("BIOS tolerante: serviço desconhecido (RAM) é registrado com argumentos e devolve ao chamador") {
    Machine m(make_disc("m10"));
    m.boot();
    m.set_bios_strict(false);

    call_slot(m, 0x06000320, 0x3333, 0x4444);

    CHECK(m.master().state().pc == 0x06003000u);
    REQUIRE(m.bios_calls().size() == 1);
    CHECK(m.bios_calls()[0].slot == 0x06000320u);
    CHECK(m.bios_calls()[0].args[0] == 0x3333u);
    CHECK(m.bios_calls()[0].args[1] == 0x4444u);
    CHECK(m.bios_calls()[0].return_address == 0x06003000u);
}

TEST_CASE("BUP_Init publica a tabela de serviços de backup e as configurações de dispositivos") {
    Machine m(make_disc("m-bup-init"));
    m.boot();
    constexpr uint32_t lib = 0x06050000, work = 0x06060000, conf = 0x06070000;
    CHECK(m.bus().read32(0x06000354) == 0);
    call_slot(m, 0x06000358, lib, work, conf);
    CHECK(m.master().state().pc == 0x06003000u);
    CHECK(m.bus().read32(0x06000354) == work);
    CHECK(m.bus().read32(work + 12) == saturn::BiosHle::backup_trap_address(3)); // BUP_Stat
    CHECK(m.bus().read32(work + 44) == saturn::BiosHle::backup_trap_address(11));
    CHECK(m.bus().read32(lib + 12) == 0); // memória da biblioteca não é tabela de ponteiros
    CHECK(m.bus().read16(conf) == 1);
    CHECK(m.bus().read16(conf + 2) == 1);
    for (uint32_t offset = 4; offset < 12; offset += 2) CHECK(m.bus().read16(conf + offset) == 0);
    CHECK_THROWS_WITH(call_slot(m, 0x06000358, 0, 0, 0), Catch::Matchers::ContainsSubstring("endereços"));
    m.master().state().pc = m.bus().read32(work + 44);
    m.master().state().pr = 0x06003000;
    CHECK_THROWS_WITH(m.step_master(), Catch::Matchers::ContainsSubstring("BUP não implementado: índice 11"));
}

TEST_CASE("gatilhos BUP são distintos dos serviços BIOS e registram índices sem colisão") {
    Machine m(make_disc("m-bup-dispatch"));
    m.boot();
    constexpr uint32_t work = 0x06060000;
    call_slot(m, 0x06000358, 0x06050000, work, 0x06070000);
    m.set_bios_strict(false);
    for (uint32_t i = 0; i < saturn::BiosHle::kBackupFunctionCount; ++i) {
        if (i == 3 || i == 5) continue;  // BUP_Stat e BUP_Read têm implementação própria
        auto& s = m.master().state();
        s.pc = m.bus().read32(work + 4 * i);
        s.pr = 0x06003000;
        m.step_master();
        CHECK(s.pc == 0x06003000u);
        CHECK(m.bios_calls().back().slot == 0xFFFF0000u + 4 * i);
    }
    REQUIRE(m.bios_calls().size() == saturn::BiosHle::kBackupFunctionCount - 2);
    CHECK(m.bus().read32(0x000005E4) != m.bus().read32(work));
    CHECK(m.bus().read32(0x06000358) != m.bus().read32(work + 3 * 4));
}

TEST_CASE("BUP_Stat e BUP_Read em SRAM vazia retornam códigos e capacidade corretos") {
    Machine m(make_disc("m-bup-empty"));
    m.boot();
    constexpr uint32_t work = 0x06060000, stat = 0x06070000;
    call_slot(m, 0x06000358, 0x06050000, work, 0x06071000);

    auto run_bup = [&](uint32_t index, uint32_t device, uint32_t size, uint32_t data) {
        auto& s = m.master().state();
        s.pc = m.bus().read32(work + 4 * index);
        s.pr = 0x06003000;
        s.r[4] = device; s.r[5] = size; s.r[6] = data;
        m.step_master();
        CHECK(s.pc == 0x06003000u);
        return s.r[0];
    };
    CHECK(run_bup(3, 2, 0, stat) == 1); // dispositivo serial ausente
    CHECK(m.bus().read32(stat) == 0);  // não grava numa estrutura desconectada
    CHECK(run_bup(3, 0, 500, stat) == 0);
    CHECK(m.bus().read32(stat + 0) == 32768);
    CHECK(m.bus().read32(stat + 4) == 512);
    CHECK(m.bus().read32(stat + 8) == 64);
    CHECK(m.bus().read32(stat + 12) == 29550);
    CHECK(m.bus().read32(stat + 16) == 510);
    CHECK(m.bus().read32(stat + 20) == (29550u - 500u) / 64);
    CHECK(run_bup(5, 0, 0x06071200, 0x06071400) == 5); // BUP_NOT_FOUND
    CHECK(run_bup(5, 1, 0x06071200, 0x06071400) == 1); // BUP_NON
    m.bus().write8(0x00180000 + 2 * 64 * 2 + 1, 0x80); // save presente
    CHECK_THROWS_WITH(run_bup(5, 0, 0, 0), Catch::Matchers::ContainsSubstring("leitura de saves"));
}

TEST_CASE("tratador registrado pelo serviço é chamado como sub-rotina (rts); a BIOS restaura registradores e faz rte") {
    Machine m(make_disc("m11"));
    m.boot();
    m.bus().write16(0x06002900, 0xE107);   // mov #7,r1   (o tratador suja r1)
    m.bus().write16(0x06002902, 0x000B);   // rts
    m.bus().write16(0x06002904, 0x0009);   // nop (slot)
    call_slot(m, 0x06000300, 0x40, 0x06002900);   // registra o tratador do VBlank-IN

    auto& s = m.master().state();
    s.pc = 0x06002100;
    s.sr = 0;
    s.r[1] = 0x1234;
    s.mach = 0xAAAA;
    m.scu().write(0xA0, 4, 0);
    m.scu().raise(saturn::Scu::Interrupt::VBlankIn);

    m.step_master();                                  // a CPU entra no vetor (gatilho do trampolim)
    m.step_master();                                  // o trampolim salva o contexto e chama o tratador
    CHECK(s.pc == 0x06002900u);
    m.step_master();                                  // mov #7,r1
    CHECK(s.r[1] == 7u);
    m.step_master();                                  // rts (com slot)
    m.step_master();                                  // gatilho de retorno: restaura e faz rte

    CHECK(s.pc == 0x06002100u);
    CHECK(s.r[1] == 0x1234u);                         // r1 restaurado
    CHECK(s.mach == 0xAAAAu);
    CHECK(s.sr == 0u);                                // SR do rte
}

TEST_CASE("BIOS: o serviço 0x5EC carrega o primeiro arquivo da raiz no endereço de leitura inicial do IP e salta para ele") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("0.BIN", "BOOT-APP").add("A.TXT", "alfa");
    auto sectors = iso.sectors();
    sectors[0][0xE2] = 0x18;
    sectors[0][0xF0] = 0x06; sectors[0][0xF1] = 0x00; sectors[0][0xF2] = 0x40; sectors[0][0xF3] = 0x80;   // 1st read address = 0x06004080
    sectors[0][0x100] = 0xAF; sectors[0][0x101] = 0xFE; sectors[0][0x102] = 0x00; sectors[0][0x103] = 0x09;
    Machine m(std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(write_single_track_disc(make_temp_dir("m12"), sectors))));
    m.boot();

    call_slot(m, 0x000005EC, 0x40, 0, 0x10);

    CHECK(m.master().state().pc == 0x06004080u);
    CHECK(m.bus().read8(0x06004080) == 'B');
    CHECK(m.bus().read8(0x06004087) == 'P');
    CHECK(m.bus().read8(0x06004088) == 0);            // não copia além do tamanho do arquivo
}

TEST_CASE("BIOS: o tamanho de leitura inicial do cabeçalho (+0xF4) limita o que é carregado") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("0.BIN", "BOOT-APP");
    auto sectors = iso.sectors();
    sectors[0][0xE2] = 0x18;
    sectors[0][0xF0] = 0x06; sectors[0][0xF1] = 0x00; sectors[0][0xF2] = 0x40; sectors[0][0xF3] = 0x80;
    sectors[0][0xF7] = 0x04;                                                                               // 1st read size = 4
    sectors[0][0x100] = 0xAF; sectors[0][0x101] = 0xFE; sectors[0][0x102] = 0x00; sectors[0][0x103] = 0x09;
    Machine m(std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(write_single_track_disc(make_temp_dir("m13"), sectors))));
    m.boot();

    call_slot(m, 0x000005EC);

    CHECK(m.bus().read8(0x06004083) == 'T');
    CHECK(m.bus().read8(0x06004084) == 0);
}

TEST_CASE("boot por software entrega o CD block como o BIOS: DCHG limpo e CMOK pendente") {
    Machine m(make_disc("m_hirq"));
    m.boot();

    const uint32_t hirq = m.bus().read16(0x05890008);
    CHECK((hirq & 0x0020) == 0u);                         // DCHG: o BIOS já tratou a troca de disco
    CHECK((hirq & 0x0001) != 0u);                         // CMOK
}

TEST_CASE("memória de backup: sai formatada e load_backup_ram restaura os saves e repõe o cabeçalho de formatação") {
    Machine m(make_disc("m-bup"));
    const auto logical = [&](uint32_t i) { return m.bus().read8(0x00180000 + 2 * i + 1); };
    const std::string signature = "BackUpRam Format";
    for (uint32_t i = 0; i < 64; ++i) CHECK(logical(i) == uint8_t(signature[i % signature.size()]));

    std::vector<uint8_t> image(Machine::kBackupRamSize, 0);   // arquivo antigo: sem cabeçalho, com um save no bloco 2
    image[2 * 128 + 1] = 0x80;
    m.load_backup_ram(image);
    CHECK(logical(0) == uint8_t('B'));
    CHECK(logical(15) == uint8_t('t'));
    CHECK(logical(128) == 0x80);
    CHECK_THROWS(m.load_backup_ram(std::vector<uint8_t>(16)));
}
