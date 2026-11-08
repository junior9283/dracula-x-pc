#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "saturn/smpc.hpp"

using saturn::Scu;
using saturn::Smpc;

namespace {
// Registradores do SMPC: bytes em offsets ímpares.
constexpr uint32_t kIreg0 = 0x01, kIreg1 = 0x03, kComreg = 0x1F, kOreg0 = 0x21, kOreg31 = 0x5F, kSr = 0x61, kSf = 0x63;

constexpr uint32_t oreg(int n) { return kOreg0 + 2 * n; }

struct SmpcRig {
    saturn::Bus bus;
    Scu scu{bus};
    Smpc smpc{scu};
    std::vector<int> vectors;

    SmpcRig() {
        scu.write(0xA0, 4, 0);  // desmascara
        scu.deliver = [this](int, int vector) { vectors.push_back(vector); scu.acknowledge(vector); };
    }
    void command(uint8_t code) {
        smpc.write(kSf, 1, 1);          // o software marca SF antes de enviar o comando
        smpc.write(kComreg, 1, code);
        smpc.advance(Smpc::kCommandCycles);   // espera o comando concluir
    }
};
}  // namespace

TEST_CASE("SSHON/SSHOFF avisam o estado do SH-2 escravo, limpam SF e geram a interrupção System Manager") {
    SmpcRig rig;
    std::vector<bool> slave;
    rig.smpc.on_slave_power = [&](bool on) { slave.push_back(on); };

    rig.command(0x02);
    CHECK(slave == std::vector<bool>{true});
    CHECK(rig.smpc.read(kSf, 1) == 0u);
    REQUIRE_FALSE(rig.vectors.empty());
    CHECK(rig.vectors.back() == 0x47);

    rig.command(0x03);
    CHECK(slave == std::vector<bool>{true, false});
}

TEST_CASE("SNDON/SNDOFF avisam o estado do 68000 de som") {
    SmpcRig rig;
    std::vector<bool> sound;
    rig.smpc.on_sound_power = [&](bool on) { sound.push_back(on); };
    rig.command(0x06);
    rig.command(0x07);
    CHECK(sound == std::vector<bool>{true, false});
}

TEST_CASE("INTBACK só de periférico entrega o pad digital em OREG (formato inferido)") {
    SmpcRig rig;
    rig.smpc.set_pad(0x12AB);
    rig.smpc.write(kIreg0, 1, 0x00);   // sem status
    rig.smpc.write(kIreg1, 1, 0x08);   // dados de periférico
    rig.command(0x10);

    CHECK(rig.smpc.read(oreg(0), 1) == 0xF1u);   // porta 1: um dispositivo
    CHECK(rig.smpc.read(oreg(1), 1) == 0x02u);   // pad digital, 2 bytes
    CHECK(rig.smpc.read(oreg(2), 1) == 0x12u);
    CHECK(rig.smpc.read(oreg(3), 1) == 0xABu);
    CHECK(rig.smpc.read(oreg(4), 1) == 0xF0u);   // porta 2: vazia
    CHECK(rig.smpc.read(kOreg31, 1) == 0x10u);   // eco do comando
}

TEST_CASE("INTBACK de status entrega o código de área em OREG9") {
    SmpcRig rig;
    rig.smpc.set_area_code(0x04);
    rig.smpc.write(kIreg0, 1, 0x01);
    rig.smpc.write(kIreg1, 1, 0x00);
    rig.command(0x10);
    CHECK(rig.smpc.read(oreg(9), 1) == 0x04u);

    SmpcRig japan;
    japan.smpc.write(kIreg0, 1, 0x01);
    japan.command(0x10);
    CHECK(japan.smpc.read(oreg(9), 1) == 0x01u);   // padrão: Japão (o disco é japonês)
}

TEST_CASE("INTBACK de status + periférico: o status vem primeiro (SR com NPE) e IREG0 bit 7 pede o pad numa segunda interrupção") {
    SmpcRig rig;
    rig.smpc.set_pad(0x34CD);
    rig.smpc.write(kIreg0, 1, 0x01);
    rig.smpc.write(kIreg1, 1, 0x08);
    rig.command(0x10);

    CHECK(rig.smpc.read(oreg(0), 1) == 0xC0u);                 // status: RTC válido + reset desabilitado (estado ao ligar)
    CHECK((rig.smpc.read(kSr, 1) & 0x40) == 0x40u);            // NPE: ainda há dados de periférico
    const size_t interrupts_before = rig.vectors.size();

    rig.smpc.write(kIreg0, 1, 0x80);                           // continua
    rig.smpc.advance(Smpc::kCommandCycles);
    CHECK(rig.vectors.size() == interrupts_before + 1);
    CHECK(rig.smpc.read(oreg(0), 1) == 0xF1u);
    CHECK(rig.smpc.read(oreg(2), 1) == 0x34u);
    CHECK(rig.smpc.read(oreg(3), 1) == 0xCDu);
    CHECK(rig.smpc.read(kSr, 1) == 0xC0u);                     // PDL + NPE: o software encerra com BREAK

    rig.smpc.write(kIreg0, 1, 0x80);                           // sem INTBACK pendente: ignorado
    rig.smpc.advance(Smpc::kCommandCycles);
    CHECK(rig.vectors.size() == interrupts_before + 1);
}

TEST_CASE("INTBACK: IREG0 bit 6 (BREAK) cancela o envio do periférico") {
    SmpcRig rig;
    rig.smpc.write(kIreg0, 1, 0x01);
    rig.smpc.write(kIreg1, 1, 0x08);
    rig.command(0x10);
    const size_t interrupts_before = rig.vectors.size();
    rig.smpc.write(kIreg0, 1, 0x40);
    rig.smpc.write(kIreg0, 1, 0x80);
    rig.smpc.advance(Smpc::kCommandCycles);
    CHECK(rig.vectors.size() == interrupts_before);
}

TEST_CASE("só os bytes ímpares existem: escrita/leitura em endereço par é ignorada") {
    SmpcRig rig;
    std::vector<bool> slave;
    rig.smpc.on_slave_power = [&](bool on) { slave.push_back(on); };

    rig.smpc.write(kSf, 1, 1);
    rig.smpc.write(kComreg - 1, 1, 0x02);          // offset par: não dispara
    CHECK(slave.empty());
    CHECK(rig.smpc.read(kOreg0 - 1, 1) == 0u);

    rig.smpc.write(kIreg0, 1, 0x5A);
    CHECK(rig.smpc.read(kIreg0 - 1, 1) == 0u);
    CHECK(rig.smpc.read(kIreg0, 1) == 0x5Au);      // IREG volta o que foi escrito
}

TEST_CASE("comando desconhecido é registrado, conclui (SF=0) e não derruba nada") {
    SmpcRig rig;
    rig.command(0x7E);
    CHECK(rig.smpc.read(kSf, 1) == 0u);
    REQUIRE(rig.smpc.unknown_commands().size() == 1);
    CHECK(rig.smpc.unknown_commands()[0] == 0x7E);
}

TEST_CASE("comandos conhecidos sem efeito observável concluem normalmente") {
    SmpcRig rig;
    for (uint8_t code : {0x16, 0x19, 0x1A, 0x08, 0x09}) {
        rig.command(code);
        CHECK(rig.smpc.read(kSf, 1) == 0u);
    }
    CHECK(rig.smpc.unknown_commands().empty());
}

TEST_CASE("acesso de 16 bits compõe o byte ímpar (leitura de OREG por word)") {
    SmpcRig rig;
    rig.smpc.set_pad(0x1234);
    rig.smpc.write(kIreg1, 1, 0x08);
    rig.command(0x10);
    CHECK(rig.smpc.read(oreg(0) - 1, 2) == 0xF1u);   // word em offset par = {byte par (0), byte ímpar}
}

TEST_CASE("o comando só conclui depois do atraso: SF continua 1 e a interrupção System Manager espera") {
    SmpcRig rig;
    rig.smpc.write(kSf, 1, 1);
    rig.smpc.write(kComreg, 1, 0x10);                   // INTBACK
    rig.smpc.advance(Smpc::kCommandCycles - 1);
    CHECK(rig.smpc.read(kSf, 1) == 1u);
    CHECK(rig.vectors.empty());

    rig.smpc.advance(1);
    CHECK(rig.smpc.read(kSf, 1) == 0u);
    REQUIRE_FALSE(rig.vectors.empty());
    CHECK(rig.vectors.back() == 0x47);
}

TEST_CASE("CKCHG352/320: desliga escravo e som, avisa a troca e só conclui com NMI depois do tempo da troca de clock") {
    SmpcRig rig;
    int clock_changes = 0, nmis = 0;
    std::vector<bool> slave, sound;
    rig.smpc.on_clock_change = [&] { ++clock_changes; };
    rig.smpc.on_nmi = [&] { ++nmis; };
    rig.smpc.on_slave_power = [&](bool on) { slave.push_back(on); };
    rig.smpc.on_sound_power = [&](bool on) { sound.push_back(on); };

    rig.command(0x0E);
    CHECK(clock_changes == 1);
    CHECK(slave == std::vector<bool>{false});
    CHECK(sound == std::vector<bool>{false});
    CHECK(rig.smpc.read(kSf, 1) == 1u);   // kCommandCycles não bastam
    CHECK(nmis == 0);
    rig.smpc.advance(Smpc::kClockChangeCycles);
    CHECK(rig.smpc.read(kSf, 1) == 0u);
    CHECK(nmis == 1);

    rig.smpc.write(kIreg0, 1, 0x01);       // INTBACK de status: OREG10 bit 6 = modo 352
    rig.smpc.write(kIreg1, 1, 0x00);
    rig.command(0x10);
    CHECK((rig.smpc.read(oreg(10), 1) & 0x40) == 0x40u);
}

TEST_CASE("NMIREQ gera NMI na conclusão; RESENAB limpa o bit de reset desabilitado do OREG0") {
    SmpcRig rig;
    int nmis = 0;
    rig.smpc.on_nmi = [&] { ++nmis; };
    rig.command(0x18);
    CHECK(nmis == 1);
    CHECK(rig.smpc.read(kSf, 1) == 0u);

    rig.command(0x19);
    rig.smpc.write(kIreg0, 1, 0x01);
    rig.smpc.write(kIreg1, 1, 0x00);
    rig.command(0x10);
    CHECK(rig.smpc.read(oreg(0), 1) == 0x80u);
    CHECK(nmis == 1);
}

namespace {
// Pede o INTBACK de status e conclui o comando.
void intback_status(SmpcRig& rig) {
    rig.smpc.write(kIreg0, 1, 0x01);
    rig.smpc.write(kIreg1, 1, 0x00);
    rig.command(0x10);
}
}  // namespace

TEST_CASE("INTBACK de status: OREG1-7 devolvem o RTC fixo de 1995-01-01 em BCD, igual a cada chamada") {
    SmpcRig rig;
    intback_status(rig);
    const uint8_t expected[7] = {0x19, 0x95, 0x01, 0x01, 0x00, 0x00, 0x00};
    for (int i = 0; i < 7; ++i) CHECK(rig.smpc.read(oreg(1 + i), 1) == expected[i]);
    CHECK(rig.smpc.read(kSr, 1) == 0u);                        // sem pedido de periférico: sem NPE
    intback_status(rig);
    for (int i = 0; i < 7; ++i) CHECK(rig.smpc.read(oreg(1 + i), 1) == expected[i]);
}

TEST_CASE("INTBACK de status: OREG10/OREG11 variam com SSHON/SNDON/CKCHG352/CDOFF") {
    SmpcRig rig;
    intback_status(rig);
    const uint32_t base10 = rig.smpc.read(oreg(10), 1), base11 = rig.smpc.read(oreg(11), 1);
    CHECK((base10 & 0x40) == 0u);      // relógio 320
    CHECK((base10 & 0x10) == 0u);      // escravo desligado
    CHECK((base10 & 0x01) == 0u);      // som desligado
    CHECK((base11 & 0x40) != 0u);      // CD ligado (CDRES)

    rig.command(0x02);                 // SSHON
    intback_status(rig);
    CHECK(rig.smpc.read(oreg(10), 1) == (base10 | 0x10));

    rig.command(0x06);                 // SNDON
    intback_status(rig);
    CHECK(rig.smpc.read(oreg(10), 1) == (base10 | 0x10 | 0x01));

    rig.command(0x09);                 // CDOFF
    intback_status(rig);
    CHECK(rig.smpc.read(oreg(11), 1) == (base11 & ~0x40u));
    CHECK(rig.smpc.read(oreg(10), 1) == (base10 | 0x10 | 0x01));   // OREG10 intacto

    rig.command(0x08);                 // CDON
    intback_status(rig);
    CHECK(rig.smpc.read(oreg(11), 1) == base11);

    rig.command(0x0E);                 // CKCHG352 desliga escravo e som e liga DOTSEL
    rig.smpc.advance(Smpc::kClockChangeCycles);
    intback_status(rig);
    CHECK(rig.smpc.read(oreg(10), 1) == (base10 | 0x40));

    rig.command(0x0F);                 // CKCHG320 volta ao relógio 320
    rig.smpc.advance(Smpc::kClockChangeCycles);
    intback_status(rig);
    CHECK(rig.smpc.read(oreg(10), 1) == base10);
}

TEST_CASE("INTBACK de status: OREG12-15 devolvem a SMEM gravada por SETSMEM (IREG0-3)") {
    SmpcRig rig;
    intback_status(rig);
    for (int i = 12; i < 16; ++i) CHECK(rig.smpc.read(oreg(i), 1) == 0u);

    for (int i = 0; i < 4; ++i) rig.smpc.write(kIreg0 + 2 * i, 1, 0xA0 + i);
    rig.command(0x17);                 // SETSMEM
    intback_status(rig);
    for (int i = 0; i < 4; ++i) CHECK(rig.smpc.read(oreg(12 + i), 1) == 0xA0u + i);
    CHECK(rig.smpc.unknown_commands().empty());
}

TEST_CASE("save/load do SMPC preserva SMEM, estado do sistema, área e INTBACK pendente") {
    SmpcRig rig;
    rig.smpc.set_area_code(0x04);
    for (int i = 0; i < 4; ++i) rig.smpc.write(kIreg0 + 2 * i, 1, 0x10 + i);
    rig.command(0x17);
    rig.command(0x02);                 // SSHON
    rig.command(0x06);                 // SNDON
    rig.command(0x09);                 // CDOFF
    rig.command(0x19);                 // RESENAB
    rig.smpc.write(kIreg0, 1, 0x01);
    rig.smpc.write(kIreg1, 1, 0x08);
    rig.command(0x10);                 // status + periférico pendente
    saturn::StateWriter w;
    rig.smpc.save(w);
    const auto bytes = w.take();

    SmpcRig fresh;
    saturn::StateReader r(bytes);
    fresh.smpc.load(r);
    intback_status(fresh);
    CHECK(fresh.smpc.read(oreg(0), 1) == 0x80u);               // reset habilitado
    CHECK(fresh.smpc.read(oreg(9), 1) == 0x04u);
    CHECK((fresh.smpc.read(oreg(10), 1) & 0x11) == 0x11u);     // escravo e som ligados
    CHECK((fresh.smpc.read(oreg(11), 1) & 0x40) == 0u);        // CD desligado
    for (int i = 0; i < 4; ++i) CHECK(fresh.smpc.read(oreg(12 + i), 1) == 0x10u + i);
}

TEST_CASE("save/load do SMPC preserva o comando em andamento: SF e a conclusão continuam") {
    SmpcRig rig;
    rig.smpc.write(kSf, 1, 1);
    rig.smpc.write(kComreg, 1, 0x10);
    rig.smpc.advance(Smpc::kCommandCycles - 5);
    saturn::StateWriter w;
    rig.smpc.save(w);
    const auto bytes = w.take();

    SmpcRig fresh;
    saturn::StateReader r(bytes);
    fresh.smpc.load(r);
    CHECK(fresh.smpc.read(kSf, 1) == 1u);
    fresh.smpc.advance(5);
    CHECK(fresh.smpc.read(kSf, 1) == 0u);
    CHECK_FALSE(fresh.vectors.empty());
}
