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
    std::string big;                            // por padrão 3000 bytes: atravessa 2 setores
    saturnfs::DiscImage disc;
    CdBlock cd;

    static saturnfs::DiscImage make_disc(const std::string& big, const std::string& tag) {
        SyntheticIso iso("T-9527G", "V1.400");
        iso.add("A.TXT", "alfa").add("DIR/B.BIN", big);
        return open_disc(iso, tag);
    }
    explicit CdRig(const std::string& tag, size_t big_bytes = 3000) : big(big_bytes, 'b'), disc(make_disc(big, tag)), cd(disc) {}

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
    CHECK(rig.cd.read(kHirqMask, 2) == 0xFFFFu);    // reset atual: todas as fontes habilitadas
    CHECK(rig.cd.irq_pending());                     // CMOK habilitado
    rig.cd.write(kHirqMask, 2, 0);                  // desabilita interrupções
    CHECK_FALSE(rig.cd.irq_pending());
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
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);                 // ChangeDirectory (0x70): raiz (id 0xFFFFFF)
    CHECK((rig.hirq() & 0x200) != 0);                   // EFLS
    auto scope = rig.cmd(0x7200);
    CHECK(scope[1] == 2u);                              // conta só "DIR" e "A.TXT" (sem "." e "..")

    rig.cmd(0x7300, 0, 0x0000, 3);                      // GetFileInfo id 3 (A.TXT)
    CHECK((rig.hirq() & 0x02) != 0);                    // DRDY
    auto info = rig.read_port(12);
    const uint32_t fad = (uint8_t(info[0]) << 24) | (uint8_t(info[1]) << 16) | (uint8_t(info[2]) << 8) | uint8_t(info[3]);
    const uint32_t size = (uint8_t(info[4]) << 24) | (uint8_t(info[5]) << 16) | (uint8_t(info[6]) << 8) | uint8_t(info[7]);
    CHECK(fad == rig.lba_of("A.TXT") + kFadOffset);
    CHECK(size == 4u);
    CHECK(uint8_t(info[10]) == 0u);                     // número de arquivo XA (sem registro XA na ISO de teste)
    CHECK(uint8_t(info[11]) == 0u);                     // atributo: arquivo
    rig.cmd(0x0600);                                    // EndDataTransfer
}

TEST_CASE("ReadDirectory pagina registros do diretório atual sem entrar no arquivo escolhido") {
    CdRig rig("cd-read-dir");
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF); // raiz
    auto initial = rig.cmd(0x7200);
    CHECK(initial[1] == 2u);
    CHECK(initial[2] == 0x0100u);  // último lote disponível
    CHECK(initial[3] == 2u);

    auto loaded = rig.cmd(0x7100, 0, 0x0000, 3); // recomeça em A.TXT (ID 3)
    CHECK(loaded[0] != 0xFF00u);
    auto scope = rig.cmd(0x7200);
    CHECK(scope[1] == 1u);
    CHECK(scope[3] == 3u);

    CHECK(rig.cmd(0x7300, 0, 0, 2)[0] == 0xFF00u); // DIR ficou fora da janela
    CHECK(rig.cmd(0x7300, 0, 0, 3)[1] == 6u);
    CHECK(rig.read_port(12).size() == 12u);
    rig.cmd(0x0600);

    CHECK(rig.cmd(0x7100, 0, 0x17FF, 0xFFF8)[0] == 0xFF00u); // ID inválido, não há troca de diretório
    CHECK(rig.cmd(0x7200)[3] == 3u); // uma requisição rejeitada não muda a janela
    CHECK(rig.cmd(0x7100, 0, 0x0000, 0)[0] != 0xFF00u); // reinício de enumeração
    CHECK(rig.cmd(0x7200)[3] == 2u);
}

TEST_CASE("GetFileInfo aceita os identificadores reservados do diretório atual e pai") {
    CdRig rig("cd-reserved-info");
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);
    for (uint16_t id : {uint16_t(0), uint16_t(1)}) {
        const auto resposta = rig.cmd(0x7300, 0, 0, id);
        CHECK(resposta[0] != 0xFF00u);
        CHECK(resposta[1] == 6u);
        const std::string info = rig.read_port(12);
        CHECK(static_cast<uint8_t>(info[10]) == 0u);   // número de arquivo XA, não o id
        CHECK((static_cast<uint8_t>(info[11]) & 2u) == 2u);
        rig.cmd(0x0600);
    }
    rig.cmd(0x7100, 0, 0, 3);
    CHECK(rig.cmd(0x7300, 0, 0, 0)[1] == 6u); // as duas entradas permanecem acessíveis
}

TEST_CASE("ReadFile de DIR/B.BIN: sai pelo conector do filtro, 2 setores no buffer, bytes iguais ao arquivo") {
    CdRig rig("cd5");
    rig.cmd(0x7000, 0, 0x0000, 2);                      // entra em DIR (id 2 na raiz)
    rig.cmd(0x3000, 0, 0x0000, 0);                      // SetCDDeviceConnection: filtro 0
    rig.cmd(0x4601, 0x0000, 0x0000, 0);                 // SetFilterConnection: filtro 0, saída verdadeira = partição 0
    rig.cmd(0x7400, 0, 0x0000, 2);                      // ReadFile: filtro 0, id 2 (B.BIN), deslocamento 0
    CHECK((rig.hirq() & 0x10) != 0);                    // PEND: fim da leitura

    auto count = rig.cmd(0x5100, 0, 0x0000, 0);         // GetSectorNumber da partição 0
    CHECK(count[3] == 2u);

    rig.cmd(0x6100, 0, 0x0000, 2);                      // GetSectorData: partição 0, 2 setores
    CHECK(rig.read_port(3000) == rig.big);
    rig.cmd(0x0600);                                    // EndDataTransfer

    rig.cmd(0x6200, 0, 0x0000, 0xFFFF);                 // DeleteSectorData: tudo
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

TEST_CASE("Play de uma faixa maior que o buffer: para com BFUL em 200 setores e retoma quando o jogo libera espaço") {
    constexpr uint32_t kSectors = 300;
    CdRig rig("cd-stream", kSectors * 2048);
    const uint32_t fad = rig.lba_of("DIR/B.BIN") + kFadOffset;
    rig.cmd(0x3000, 0, 0x0000, 0);                      // conector -> filtro 0
    rig.cmd(0x4601, 0x0000, 0x0000, 0);                 // filtro 0 -> partição 0
    rig.cmd(0x1000 | (0x80 | (fad >> 16)), fad & 0xFFFF, 0x0080, kSectors);   // Play: FAD, 300 setores

    CHECK(rig.cmd(0x5100, 0, 0x0000, 0)[3] == 200u);
    CHECK((rig.hirq() & 0x08) != 0);                    // BFUL
    CHECK((rig.hirq() & 0x10) == 0);                    // ainda não terminou

    rig.cmd(0x6200, 0, 0x0000, 0xFFFF);                 // DeleteSectorData: tudo
    CHECK(rig.cmd(0x5100, 0, 0x0000, 0)[3] == 100u);    // a leitura continuou de onde parou
    CHECK((rig.hirq() & 0x10) != 0);                    // PEND: fim da faixa pedida
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
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);
    rig.cmd(0x7300, 0, 0x0000, 3);                      // 12 bytes de informação
    const uint32_t first = rig.cd.data_port().read(0, 2);
    CHECK(first == ((rig.lba_of("A.TXT") + kFadOffset) >> 16));   // palavra alta do FAD
    for (int i = 0; i < 10; ++i) rig.cd.data_port().read(0, 4);   // esgota
    CHECK(rig.cd.data_port().read(0, 4) == 0u);
}

TEST_CASE("GetTOC (0x02): 102 entradas de 4 bytes pela porta de dados; faixa 1 de dados em FAD 150 e lead-out depois do último setor") {
    CdRig rig("cd9");
    auto r = rig.cmd(0x0200);
    CHECK(r[1] == 204u);                                // 408 bytes = 204 palavras
    CHECK((rig.hirq() & 0x02) != 0);                    // DRDY
    auto toc = rig.read_port(408);
    auto entry = [&](int i) { return (uint32_t(uint8_t(toc[4*i])) << 24) | (uint8_t(toc[4*i+1]) << 16) | (uint8_t(toc[4*i+2]) << 8) | uint8_t(toc[4*i+3]); };
    CHECK(entry(0) == (0x41000000u | kFadOffset));      // faixa 1: controle/endereço 0x41, FAD 150
    CHECK(entry(1) == 0xFFFFFFFFu);                      // faixa 2 inexistente
    CHECK(entry(99) == 0x41010000u);                     // A0: primeira faixa = 1
    CHECK(entry(100) == 0x41010000u);                    // A1: última faixa = 1
    CHECK((entry(101) & 0xFFFFFF) == rig.disc.sector_count() + kFadOffset);   // A2: lead-out
}

TEST_CASE("GetSessionInfo (0x03): sessão 0 = número de sessões e lead-out; sessão 1 = primeira faixa e FAD inicial 0") {
    CdRig rig("cd10");
    auto all = rig.cmd(0x0300);
    const uint32_t leadout = rig.disc.sector_count() + kFadOffset;
    CHECK((all[2] >> 8) == 1u);
    CHECK(all[2] == uint16_t((1u << 8) | ((leadout >> 16) & 0xFF)));
    CHECK(all[3] == uint16_t(leadout & 0xFFFF));

    auto first = rig.cmd(0x0301);
    CHECK((first[2] >> 8) == 1u);                       // primeira faixa
    CHECK(first[3] == 0u);                              // FAD inicial da sessão 1 = 0: o jogo soma 0xA6 (150 + 16) para achar o descritor ISO
}

TEST_CASE("on_command recebe o comando e a resposta de cada execução") {
    CdRig rig("cd11");
    std::vector<std::array<uint16_t, 4>> commands, replies;
    rig.cd.on_command = [&](const std::array<uint16_t, 4>& c, const std::array<uint16_t, 4>& r) { commands.push_back(c); replies.push_back(r); };

    rig.cmd(0x0100);
    rig.cmd(0x9900, 1, 2, 3);

    REQUIRE(commands.size() == 2);
    CHECK(commands[0][0] == 0x0100u);
    CHECK(replies[0][1] == 0x0201u);
    CHECK(commands[1] == std::array<uint16_t, 4>{0x9900, 1, 2, 3});
    CHECK(replies[1][0] == 0xFF00u);
}

TEST_CASE("o drive atualiza o subcódigo Q periodicamente: advance() liga HIRQ.SCDQ a cada 1/75 s") {
    CdRig rig("cd12");
    CHECK((rig.hirq() & 0x0400) == 0u);

    rig.cd.advance(CdBlock::kCyclesPerSubcodeFrame - 1);
    CHECK((rig.hirq() & 0x0400) == 0u);
    rig.cd.advance(1);
    CHECK((rig.hirq() & 0x0400) != 0u);

    rig.cd.write(kHirq, 2, ~0x0400u & 0xFFFF);    // o software reconhece
    CHECK((rig.hirq() & 0x0400) == 0u);
    rig.cd.advance(CdBlock::kCyclesPerSubcodeFrame * 3);
    CHECK((rig.hirq() & 0x0400) != 0u);            // e ele volta
}

TEST_CASE("DATATRNS no offset 0 dos registradores lê a mesma fila de transferência da porta de dados") {
    CdRig rig("cd_datatrns");
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);                 // ChangeDirectory: raiz
    rig.cmd(0x7300, 0, 0x0000, 3);                      // GetFileInfo id 3 (A.TXT): 12 bytes = 6 palavras
    const uint32_t fad = rig.lba_of("A.TXT") + kFadOffset;
    CHECK(rig.cd.read(0x00, 2) == (fad >> 16));         // 1ª palavra de 16 bits: FAD alto
    CHECK(rig.cd.read(0x00, 2) == (fad & 0xFFFF));
    CHECK(rig.cd.read(0x00, 2) == 0u);                  // tamanho alto
    CHECK(rig.cd.read(0x00, 2) == 4u);
    rig.cd.read(0x00, 2); rig.cd.read(0x00, 2);
    auto end = rig.cmd(0x0600);                         // EndDataTransfer devolve as palavras já lidas
    CHECK(end[1] == 6u);
}

TEST_CASE("SetFilterSubheaderConditions (0x42) é aceito e GetFilterSubheader (0x43) devolve os valores") {
    CdRig rig("cd_subheader");
    auto set = rig.cmd(0x4203, 0x1122, 0x0500 | 0x0007, 0x3344);   // filtro 5: canal 3, máscaras 0x11/0x22, nº de arquivo 7, valores 0x33/0x44
    CHECK(set[0] != 0xFF00u);
    CHECK((rig.hirq() & 0x40) != 0);                    // ESEL
    auto get = rig.cmd(0x4300, 0, 0x0500, 0);
    CHECK(get[0] != 0xFF00u);
    CHECK(get[0] == uint16_t((0x01 << 8) | 0x03));      // status | canal
    CHECK(get[1] == 0x3344u);                           // valores de submodo e codificação
    CHECK(get[2] == 0x0007u);                           // nº de arquivo
    CHECK(get[3] == 0x1122u);                           // máscaras
}

TEST_CASE("relatório periódico: no tick do SCDQ o drive reescreve CR1–CR4 com o status e o bit PERI (0x20)") {
    CdRig rig("cd_peri");
    auto reply = rig.cmd(0x0000);                       // GetStatus: resposta de comando, sem PERI
    CHECK((reply[0] & 0x2000) == 0u);

    rig.cd.advance(CdBlock::kCyclesPerSubcodeFrame);
    const uint16_t cr1 = rig.cd.read(kCr1, 2);
    CHECK((cr1 & 0x2000) != 0u);                        // PERI
    CHECK((cr1 >> 8 & 0x0F) == 0x01u);                  // continua em pausa
    CHECK(rig.cd.read(kCr4, 2) == kFadOffset);          // posição atual (FAD 150)
}

TEST_CASE("relatório periódico não corrompe a resposta de um comando recém-executado") {
    CdRig rig("cd_peri2");
    rig.cd.advance(CdBlock::kCyclesPerSubcodeFrame - 10);
    rig.cd.write(kCr1, 2, 0x0000); rig.cd.write(kCr2, 2, 0); rig.cd.write(kCr3, 2, 0); rig.cd.write(kCr4, 2, 0);   // comando
    rig.cd.advance(20);                                 // o tick cai logo após o comando
    CHECK((rig.cd.read(kCr1, 2) & 0x2000) == 0u);       // a resposta do comando permanece intacta
}

TEST_CASE("SetCDDeviceConnection (0x30) sinaliza ESEL ao terminar, como as demais operações de seletor") {
    CdRig rig("cd_esel30");
    rig.cd.write(kHirq, 2, ~0x0040u & 0xFFFF);         // o software limpa o bit que vai esperar
    CHECK((rig.hirq() & 0x0040) == 0u);
    rig.cmd(0x3000, 0, 0x0000, 0);                      // conecta o CD ao filtro 0
    CHECK((rig.hirq() & 0x0040) != 0u);
}

TEST_CASE("CalcActualSize (0x52) sinaliza ESEL quando termina o cálculo") {
    CdRig rig("cd_esel52");
    rig.cd.write(kHirq, 2, ~0x0040u & 0xFFFF);
    rig.cmd(0x5200, 0, 0, 1);                           // calcula o tamanho de 1 setor a partir do 0
    CHECK((rig.hirq() & 0x0040) != 0u);
    auto size = rig.cmd(0x5300);                        // GetActualSize
    CHECK(size[1] == 1024u);                            // 1 setor de 2048 bytes = 1024 palavras
}
