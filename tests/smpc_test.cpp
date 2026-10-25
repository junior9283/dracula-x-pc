#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "saturn/smpc.hpp"

using saturn::Scu;
using saturn::Smpc;

namespace {
// Registradores do SMPC: bytes em offsets ímpares.
constexpr uint32_t kIreg0 = 0x01, kIreg1 = 0x03, kComreg = 0x1F, kOreg0 = 0x21, kOreg31 = 0x5F, kSf = 0x63;

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
    for (uint8_t code : {0x0E, 0x0F, 0x16, 0x18, 0x19, 0x1A, 0x08, 0x09}) {
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
