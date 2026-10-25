#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

#include "iso_builder.hpp"
#include "saturn/cdblock.hpp"
#include "saturnfs/iso9660.hpp"

using saturn::CdBlock;
using namespace testsupport;

namespace {
constexpr uint32_t kHirq = 0x08, kHirqMask = 0x0C, kCr1 = 0x18, kCr2 = 0x1C, kCr3 = 0x20, kCr4 = 0x24;
constexpr uint32_t kFadOffset = 150;

struct CdRig {
    std::string big = std::string(3000, 'b');   // atravessa 2 setores
    saturnfs::DiscImage disc;
    CdBlock cd;

    static saturnfs::DiscImage make_disc(const std::string& big, const std::string& tag) {
        SyntheticIso iso("T-9527G", "V1.400");
        iso.add("A.TXT", "alfa").add("DIR/B.BIN", big);
        return open_disc(iso, tag);
    }
    explicit CdRig(const std::string& tag) : disc(make_disc(big, tag)), cd(disc) {}

    using Reply = std::array<uint16_t, 4>;
    // Escreve CR1–CR4 (CR4 por último dispara o comando) e devolve a resposta.
    Reply cmd(uint16_t cr1, uint16_t cr2 = 0, uint16_t cr3 = 0, uint16_t cr4 = 0) {
        cd.write(kCr1, 2, cr1); cd.write(kCr2, 2, cr2); cd.write(kCr3, 2, cr3); cd.write(kCr4, 2, cr4);
        return {uint16_t(cd.read(kCr1, 2)), uint16_t(cd.read(kCr2, 2)), uint16_t(cd.read(kCr3, 2)), uint16_t(cd.read(kCr4, 2))};
    }
    uint32_t hirq() { return cd.read(kHirq, 2); }

    // Lê `n` bytes da porta de dados (palavras de 32 bits).
    std::string read_port(size_t n) {
        std::string out;
        for (size_t i = 0; i < n; i += 4) {
            const uint32_t w = cd.data_port().read(0, 4);
            for (int b = 0; b < 4 && out.size() < n; ++b) out.push_back(static_cast<char>((w >> (24 - 8 * b)) & 0xFF));
        }
        return out;
    }
    uint32_t lba_of(const std::string& path) {
        for (const auto& f : saturnfs::list_files(disc)) if (f.path == path) return f.lba;
        return 0;
    }
};
}  // namespace

TEST_CASE("GetStatus: pausa na posição inicial (FAD 150) com disco de dados e CMOK em HIRQ") {
    CdRig rig("cd1");
    auto r = rig.cmd(0x0000);
    CHECK((r[0] >> 8) == 0x01u);       // PAUSE
    CHECK(r[1] == 0x4101u);            // ctrl/addr 0x41, faixa 1
    CHECK(r[3] == kFadOffset);
    CHECK((rig.hirq() & 1) == 1u);     // CMOK
}

TEST_CASE("HIRQ: escrever 0 limpa o bit; a máscara é legível e escrevível; irq_pending reflete HIRQ & máscara") {
    CdRig rig("cd2");
    rig.cmd(0x0000);
    CHECK_FALSE(rig.cd.irq_pending());              // máscara 0
    rig.cd.write(kHirqMask, 2, 0x0001);
    CHECK(rig.cd.read(kHirqMask, 2) == 1u);
    CHECK(rig.cd.irq_pending());
    rig.cd.write(kHirq, 2, ~1u & 0xFFFF);           // zera CMOK
    CHECK((rig.hirq() & 1) == 0u);
    CHECK_FALSE(rig.cd.irq_pending());
}

TEST_CASE("GetHardwareInfo devolve as constantes de versão") {
    CdRig rig("cd3");
    auto r = rig.cmd(0x0100);
    CHECK(r[1] == 0x0201u);
    CHECK(r[3] == 0x0400u);
}

TEST_CASE("ChangeDirectory na raiz + GetFileSystemScope: número de entradas; GetFileInfo por id") {
    CdRig rig("cd4");
    rig.cmd(0x6000, 0, 0x00FF, 0xFFFF);                 // ChangeDirectory: raiz (id 0xFFFFFF)
    CHECK((rig.hirq() & 0x200) != 0);                   // EFLS
    auto scope = rig.cmd(0x6200);
    CHECK(scope[1] == 4u);                              // ".", "..", "DIR", "A.TXT"

    rig.cmd(0x6300, 0, 0x0000, 3);                      // GetFileInfo id 3 (A.TXT)
    CHECK((rig.hirq() & 0x02) != 0);                    // DRDY
    auto info = rig.read_port(12);
    const uint32_t fad = (uint8_t(info[0]) << 24) | (uint8_t(info[1]) << 16) | (uint8_t(info[2]) << 8) | uint8_t(info[3]);
    const uint32_t size = (uint8_t(info[4]) << 24) | (uint8_t(info[5]) << 16) | (uint8_t(info[6]) << 8) | uint8_t(info[7]);
    CHECK(fad == rig.lba_of("A.TXT") + kFadOffset);
    CHECK(size == 4u);
    CHECK(uint8_t(info[10]) == 3u);                     // número do arquivo
    CHECK(uint8_t(info[11]) == 0u);                     // atributo: arquivo
    rig.cmd(0x0600);                                    // EndDataTransfer
}

TEST_CASE("ReadFile de DIR/B.BIN: sai pelo conector do filtro, 2 setores no buffer, bytes iguais ao arquivo") {
    CdRig rig("cd5");
    rig.cmd(0x6000, 0, 0x0000, 2);                      // entra em DIR (id 2 na raiz)
    rig.cmd(0x3000, 0, 0x0000, 0);                      // SetCDDeviceConnection: filtro 0
    rig.cmd(0x4601, 0x0000, 0x0000, 0);                 // SetFilterConnection: filtro 0, saída verdadeira = partição 0
    rig.cmd(0x6400, 0, 0x0000, 2);                      // ReadFile: filtro 0, id 2 (B.BIN), deslocamento 0
    CHECK((rig.hirq() & 0x10) != 0);                    // PEND: fim da leitura

    auto count = rig.cmd(0x5100, 0, 0x0000, 0);         // GetSectorNumber da partição 0
    CHECK(count[3] == 2u);

    rig.cmd(0x5600, 0, 0x0000, 2);                      // GetSectorData: partição 0, 2 setores
    CHECK(rig.read_port(3000) == rig.big);
    rig.cmd(0x0600);                                    // EndDataTransfer

    rig.cmd(0x5700, 0, 0x0000, 0xFFFF);                 // DeleteSectorData: tudo
    CHECK(rig.cmd(0x5100, 0, 0x0000, 0)[3] == 0u);
}

TEST_CASE("Play em modo FAD roteia os setores e o filtro de faixa de FAD descarta os de fora") {
    CdRig rig("cd6");
    const uint32_t fad = rig.lba_of("DIR/B.BIN") + kFadOffset;
    rig.cmd(0x3000, 0, 0x0000, 0);                      // conector -> filtro 0
    rig.cmd(0x4601, 0x0000, 0x0000, 0);                 // filtro 0 -> partição 0
    // Faixa de FAD do filtro 0: só o primeiro dos 2 setores; modo 0x40 liga o filtro de faixa.
    rig.cmd(0x4000 | (fad >> 16), fad & 0xFFFF, 0x0000 | 0x0000, 1);
    rig.cmd(0x4440, 0, 0x0000, 0);                      // SetFilterMode: filtro 0, modo 0x40
    rig.cmd(0x1000 | (0x80 | (fad >> 16)), fad & 0xFFFF, 0x0080, 2);   // Play: FAD, 2 setores

    CHECK(rig.cmd(0x5100, 0, 0x0000, 0)[3] == 1u);      // só 1 passou
    CHECK((rig.hirq() & 0x04) != 0);                    // CSCT
    CHECK((rig.hirq() & 0x10) != 0);                    // PEND
}

TEST_CASE("comando desconhecido: resposta 0xFF00 (rejeitado), registrado, e CMOK ainda sinalizado") {
    CdRig rig("cd7");
    auto r = rig.cmd(0x9900);
    CHECK(r[0] == 0xFF00u);
    REQUIRE(rig.cd.unknown_commands().size() == 1);
    CHECK(rig.cd.unknown_commands()[0] == 0x99);
    CHECK((rig.hirq() & 1) == 1u);
}

TEST_CASE("porta de dados: acessos de 16 bits e leitura além do fim devolvem 0") {
    CdRig rig("cd8");
    rig.cmd(0x6000, 0, 0x00FF, 0xFFFF);
    rig.cmd(0x6300, 0, 0x0000, 3);                      // 12 bytes de informação
    const uint32_t first = rig.cd.data_port().read(0, 2);
    CHECK(first == ((rig.lba_of("A.TXT") + kFadOffset) >> 16));   // palavra alta do FAD
    for (int i = 0; i < 10; ++i) rig.cd.data_port().read(0, 4);   // esgota
    CHECK(rig.cd.data_port().read(0, 4) == 0u);
}
