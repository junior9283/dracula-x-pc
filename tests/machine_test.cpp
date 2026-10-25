#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>

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
void call_slot(Machine& m, uint32_t slot, uint32_t r4 = 0, uint32_t r5 = 0) {
    constexpr uint32_t kReturn = 0x06003000;
    m.bus().write16(kReturn, 0x0009);
    auto& s = m.master().state();
    s.pc = m.bus().read32(slot);
    s.pr = kReturn;
    s.r[4] = r4; s.r[5] = r5;
    m.step_master();
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

    CHECK(m.bus().read32(saturn::BiosHle::kHandlerTable + 0x40 * 4) == 0x060022E4u);
    CHECK(m.master().state().pc == 0x06003000u);          // retornou ao chamador (PR)
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

    CHECK_THROWS_WITH(call_slot(m, 0x06000310), Catch::Matchers::ContainsSubstring("0x06000310"));
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

TEST_CASE("o SH-2 escravo só roda depois do SSHON") {
    Machine m(make_disc("m7"));
    m.boot();
    CHECK_FALSE(m.slave_running());

    m.smpc().write(0x63, 1, 1);
    m.smpc().write(0x1F, 1, 0x02);                          // SSHON
    CHECK(m.slave_running());
    m.smpc().write(0x63, 1, 1);
    m.smpc().write(0x1F, 1, 0x03);                          // SSHOFF
    CHECK_FALSE(m.slave_running());
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

    m.run_cycles(saturn::VideoTiming::kCyclesPerFrame);

    CHECK(m.frame() == 1);
    CHECK(vblanks >= 1);
}

TEST_CASE("BIOS: a tabela de ponteiros da ROM (0x5E4) aponta para um gatilho que, no modo estrito, para com o endereço na mensagem") {
    Machine m(make_disc("m9"));
    m.boot();

    CHECK_THROWS_WITH(call_slot(m, 0x000005E4), Catch::Matchers::ContainsSubstring("0x000005E4"));
}

TEST_CASE("BIOS tolerante: serviço desconhecido (ROM ou RAM) é registrado com argumentos e devolve ao chamador") {
    Machine m(make_disc("m10"));
    m.boot();
    m.set_bios_strict(false);

    call_slot(m, 0x000005E8, 0x1111, 0x2222);
    call_slot(m, 0x06000310, 0x3333, 0x4444);

    CHECK(m.master().state().pc == 0x06003000u);
    REQUIRE(m.bios_calls().size() == 2);
    CHECK(m.bios_calls()[0].slot == 0x5E8u);
    CHECK(m.bios_calls()[0].args[0] == 0x1111u);
    CHECK(m.bios_calls()[0].args[1] == 0x2222u);
    CHECK(m.bios_calls()[1].slot == 0x06000310u);
    CHECK(m.bios_calls()[1].return_address == 0x06003000u);
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
