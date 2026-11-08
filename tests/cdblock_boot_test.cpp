// CD block: comandos usados no boot pelo BIOS real (autenticação, Reset Selector, Init por software, Abort File,
// Get File Info com extensão XA, Get Copy Error) e save/load do estado. Os discos são sintéticos.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>

#include "iso_builder.hpp"
#include "saturn/cdblock.hpp"
#include "saturn/state.hpp"

using saturn::CdBlock;
using namespace testsupport;

namespace {
constexpr uint32_t kHirq = 0x08, kCr1 = 0x18, kCr2 = 0x1C, kCr3 = 0x20, kCr4 = 0x24;
constexpr uint32_t kFadOffset = 150;
constexpr uint16_t kRejected = 0xFF00;
constexpr uint32_t kEfls = 0x0200, kMped = 0x0800, kEcpy = 0x0100, kEhst = 0x0080, kEsel = 0x0040;
constexpr int kRootSector = 17;   // setor do diretório raiz no SyntheticIso

// Registro ISO9660 de arquivo com área de uso do sistema XA (14 bytes depois do nome, alinhada a 2).
struct XaFile { std::string name; uint8_t unit_size, gap_size, xa_attributes, file_number; char tag0 = 'X', tag1 = 'A'; };

// Acrescenta os registros ao diretório raiz do SyntheticIso (depois dos que já existem).
void append_xa_records(Sector& root, const std::vector<XaFile>& files) {
    size_t pos = 0;
    while (root[pos] != 0) pos += root[pos];
    for (const XaFile& f : files) {
        const size_t system_use = 33 + (f.name.size() | 1u), len = system_use + 14;
        root[pos] = static_cast<uint8_t>(len);
        put_both32(root, pos + 2, 18);
        put_both32(root, pos + 10, 10);
        root[pos + 26] = f.unit_size;
        root[pos + 27] = f.gap_size;
        root[pos + 32] = static_cast<uint8_t>(f.name.size());
        std::memcpy(root.data() + pos + 33, f.name.data(), f.name.size());
        root[pos + system_use + 4] = f.xa_attributes;
        root[pos + system_use + 6] = static_cast<uint8_t>(f.tag0);
        root[pos + system_use + 7] = static_cast<uint8_t>(f.tag1);
        root[pos + system_use + 8] = f.file_number;
        pos += len;
    }
}

struct BootRig {
    saturnfs::DiscImage disc;
    CdBlock cd;

    explicit BootRig(const std::string& tag, size_t big_bytes = 3000, const std::vector<XaFile>& xa = {}, bool saturn_header = true)
        : disc(make_disc(tag, big_bytes, xa, saturn_header)), cd(disc) {}

    static saturnfs::DiscImage make_disc(const std::string& tag, size_t big_bytes, const std::vector<XaFile>& xa, bool saturn_header) {
        SyntheticIso iso("T-9527G", "V1.400");
        iso.add("A.TXT", "alfa").add("DIR/B.BIN", std::string(big_bytes, 'b'));
        auto sectors = iso.sectors();
        append_xa_records(sectors[kRootSector], xa);
        if (!saturn_header) std::fill(sectors[0].begin(), sectors[0].begin() + 16, uint8_t{0x20});
        return saturnfs::DiscImage::open(write_single_track_disc(make_temp_dir(tag), sectors));
    }

    using Reply = std::array<uint16_t, 4>;
    Reply cmd(uint16_t cr1, uint16_t cr2 = 0, uint16_t cr3 = 0, uint16_t cr4 = 0) {
        cd.write(kCr1, 2, cr1); cd.write(kCr2, 2, cr2); cd.write(kCr3, 2, cr3); cd.write(kCr4, 2, cr4);
        return {uint16_t(cd.read(kCr1, 2)), uint16_t(cd.read(kCr2, 2)), uint16_t(cd.read(kCr3, 2)), uint16_t(cd.read(kCr4, 2))};
    }
    uint32_t hirq() { return cd.read(kHirq, 2); }
    void clear_hirq() { cd.write(kHirq, 2, 0); }

    // Comandos de seletor.
    void connect(int filter) { cmd(0x3000, 0, uint16_t(filter << 8)); }
    uint16_t connection() { return cmd(0x3100)[2] >> 8; }
    void filter_outputs(int filter, int true_out, int false_out) {
        cmd(0x4603, uint16_t((true_out << 8) | false_out), uint16_t(filter << 8));
    }
    void filter_range(int filter, uint32_t start, uint32_t count) {
        cmd(0x4000 | (start >> 16), start & 0xFFFF, uint16_t((filter << 8) | (count >> 16)), count & 0xFFFF);
    }
    void filter_mode(int filter, uint8_t mode) { cmd(0x4400 | mode, 0, uint16_t(filter << 8)); }
    // Filtro com faixa de FAD que nunca casa: os setores seguem a saída falsa.
    void make_filter_reject(int filter) { filter_range(filter, 0x7000, 1); filter_mode(filter, 0x40); }
    uint16_t sectors_in(int partition) { return cmd(0x5100, 0, uint16_t(partition << 8))[3]; }
    // Play em modo FAD.
    void play(uint32_t fad, uint32_t count) { cmd(0x1000 | 0x80 | (fad >> 16), fad & 0xFFFF, 0x0080 | ((count >> 16) & 0x7F), count & 0xFFFF); }
    // Liga o CD ao filtro e lê os primeiros `sectors` setores do disco (FAD 150..).
    void feed(int filter, uint32_t sectors = 1) { connect(filter); play(kFadOffset, sectors); }

    std::string read_port(size_t n) {
        std::string out;
        for (size_t i = 0; i < n; i += 4) {
            const uint32_t w = cd.data_port().read(0, 4);
            for (int b = 0; b < 4 && out.size() < n; ++b) out.push_back(static_cast<char>((w >> (24 - 8 * b)) & 0xFF));
        }
        return out;
    }
};
}  // namespace

// ---- Autenticação (0xE0 / 0xE1)

TEST_CASE("autenticação: sem comando 0xE0 o tipo é 0; disco Saturn vira tipo 4 e sinaliza EFLS") {
    BootRig rig("cdb-auth4");
    CHECK(rig.cmd(0xE100)[1] == 0u);
    rig.clear_hirq();
    const auto reply = rig.cmd(0xE000, 0);
    CHECK(reply[0] != kRejected);
    CHECK((rig.hirq() & kEfls) != 0u);
    CHECK((rig.hirq() & kMped) == 0u);
    CHECK(rig.cmd(0xE100)[1] == 4u);
}

TEST_CASE("autenticação: disco sem \"SEGA SEGASATURN \" no setor 0 vira tipo 2") {
    BootRig rig("cdb-auth2", 3000, {}, false);
    rig.cmd(0xE000, 0);
    CHECK(rig.cmd(0xE100)[1] == 2u);
}

TEST_CASE("autenticação: authenticate_disc() direto equivale ao comando 0xE0") {
    BootRig rig("cdb-auth-direct");
    rig.cd.authenticate_disc();
    CHECK(rig.cmd(0xE100)[1] == 4u);
}

TEST_CASE("autenticação com CR2 = 1 é a placa MPEG: sinaliza MPED, não toca o tipo do disco e 0xE1 com CR2 != 0 devolve 0") {
    BootRig rig("cdb-authmpeg");
    rig.clear_hirq();
    const auto reply = rig.cmd(0xE000, 1);
    CHECK(reply[0] != kRejected);
    CHECK((rig.hirq() & kMped) != 0u);
    CHECK((rig.hirq() & kEfls) == 0u);
    CHECK(rig.cmd(0xE100)[1] == 0u);          // o disco continua sem autenticar

    rig.cmd(0xE000, 0);
    CHECK(rig.cmd(0xE100)[1] == 4u);
    CHECK(rig.cmd(0xE101, 1)[1] == 0u);       // consulta da placa MPEG
    rig.cmd(0xE000, 1);
    CHECK(rig.cmd(0xE100)[1] == 4u);          // a autenticação MPEG não apaga a do disco
}

// ---- Get Copy Error

TEST_CASE("Get Copy Error (0x67): sem erro, com o status no byte alto de CR1") {
    BootRig rig("cdb-copyerr");
    const auto reply = rig.cmd(0x6700);
    CHECK(reply == BootRig::Reply{0x0100, 0, 0, 0});    // PAUSE, sem erro
}

// ---- Reset Selector

TEST_CASE("Reset Selector sem flags esvazia só a partição do filtro; 0x04 esvazia todas") {
    BootRig rig("cdb-rs-part");
    rig.feed(0, 3);
    rig.feed(1, 2);
    REQUIRE(rig.sectors_in(0) == 3u);
    REQUIRE(rig.sectors_in(1) == 2u);

    rig.clear_hirq();
    const auto reply = rig.cmd(0x4800, 0, 1 << 8);      // sem flags, filtro 1
    CHECK(reply[0] != kRejected);
    CHECK((rig.hirq() & kEsel) != 0u);
    CHECK(rig.sectors_in(0) == 3u);
    CHECK(rig.sectors_in(1) == 0u);

    rig.cmd(0x4804);                                    // flag 0x04: todas as partições
    CHECK(rig.sectors_in(0) == 0u);
}

TEST_CASE("Reset Selector sem flags com filtro fora de 0..23 não rejeita nem mexe nas partições") {
    BootRig rig("cdb-rs-range");
    rig.feed(0, 2);
    const auto reply = rig.cmd(0x4800, 0, 30 << 8);
    CHECK(reply[0] != kRejected);
    CHECK(rig.sectors_in(0) == 2u);
}

TEST_CASE("Reset Selector 0x10 zera as condições dos filtros mas mantém as saídas") {
    BootRig rig("cdb-rs-conds");
    rig.cmd(0x4203, 0x1122, (2 << 8) | 0x07, 0x3344);   // subheader do filtro 2
    rig.filter_outputs(2, 5, 7);
    rig.make_filter_reject(2);                          // faixa + modo
    rig.feed(2);
    REQUIRE(rig.sectors_in(7) == 1u);                   // condição ativa: a saída falsa (7) recebe
    REQUIRE(rig.sectors_in(5) == 0u);

    rig.cmd(0x4810);
    CHECK(rig.cmd(0x4300, 0, 2 << 8) == BootRig::Reply{0x0100, 0, 0, 0});   // canal, valores, nº de arquivo, máscaras = 0
    rig.cmd(0x4804);
    rig.feed(2);                                        // sem modo de faixa: passa; a saída verdadeira (5) foi mantida
    CHECK(rig.sectors_in(5) == 1u);
    CHECK(rig.sectors_in(7) == 0u);
}

TEST_CASE("Reset Selector 0x20 desliga a conexão do CD e as saídas falsas que apontam para filtros") {
    BootRig rig("cdb-rs-inputs");
    rig.filter_outputs(2, 2, 7);
    rig.filter_outputs(3, 3, 0xFF);                     // 0xFF não é filtro: fica como está
    rig.make_filter_reject(2);
    rig.connect(2);
    REQUIRE(rig.connection() == 2u);

    rig.cmd(0x4820);
    CHECK(rig.connection() == 0xFFu);

    rig.feed(2);                                        // religa: o filtro 2 rejeita e a saída falsa foi desligada
    CHECK(rig.sectors_in(7) == 0u);
    CHECK(rig.sectors_in(2) == 0u);
}

TEST_CASE("Reset Selector 0x20 desliga a conexão do CD qualquer que seja o filtro") {
    BootRig rig("cdb-rs-inputs2");
    rig.connect(23);
    rig.cmd(0x4820);
    CHECK(rig.connection() == 0xFFu);
}

TEST_CASE("Reset Selector 0x40 devolve a saída verdadeira do filtro i à partição i") {
    BootRig rig("cdb-rs-true");
    rig.filter_outputs(2, 9, 0xFF);
    rig.feed(2);
    REQUIRE(rig.sectors_in(9) == 1u);
    rig.cmd(0x4804);                                    // esvazia

    rig.cmd(0x4840);
    rig.feed(2);
    CHECK(rig.sectors_in(2) == 1u);
    CHECK(rig.sectors_in(9) == 0u);
}

TEST_CASE("Reset Selector 0x80 desliga todas as saídas falsas") {
    BootRig rig("cdb-rs-false");
    rig.filter_outputs(2, 2, 7);
    rig.make_filter_reject(2);
    rig.cmd(0x4880);
    rig.feed(2);
    CHECK(rig.sectors_in(2) == 0u);
    CHECK(rig.sectors_in(7) == 0u);
}

// ---- Filtros fora de 0..23

TEST_CASE("filtro fora de 0..23: Get Filter Subheader e os diretórios rejeitam; os comandos de ajuste são ignorados") {
    BootRig rig("cdb-badfilter");
    CHECK(rig.cmd(0x4300, 0, 24 << 8)[0] == kRejected);
    CHECK(rig.cmd(0x7000, 0, (24 << 8) | 0xFF, 0xFFFF)[0] == kRejected);
    CHECK(rig.cmd(0x7100, 0, 24 << 8, 2)[0] == kRejected);
    CHECK(rig.cmd(0x4300, 0, 23 << 8)[0] != kRejected);   // limite superior válido

    // Ajustes em filtro inexistente: aceitos mas sem efeito (não corrompem outro filtro).
    CHECK(rig.cmd(0x4203, 0x1122, (24 << 8) | 7, 0x3344)[0] != kRejected);
    CHECK(rig.cmd(0x4401, 0, 24 << 8)[0] != kRejected);
    CHECK(rig.cmd(0x4603, 0x0102, 24 << 8)[0] != kRejected);
    CHECK(rig.cmd(0x4000, 1, (24 << 8), 1)[0] != kRejected);
    for (int f : {0, 23}) CHECK(rig.cmd(0x4300, 0, uint16_t(f << 8)) == BootRig::Reply{0x0100, 0, 0, 0});
    CHECK(rig.sectors_in(24) == 0u);
}

// ---- Init

TEST_CASE("Init com bit 0 (reset por software) limpa buffer, transferência, diretório, filtros e conexão; HIRQ ganha MPED|EFLS|ECPY|EHST|ESEL") {
    BootRig rig("cdb-init-soft");
    rig.cmd(0x7000, 0, 0x0000, 2);                      // entra em DIR: janela com só B.BIN
    REQUIRE(rig.cmd(0x7200)[1] == 1u);
    rig.cmd(0x4203, 0x1122, (5 << 8) | 0x07, 0x3344);
    rig.filter_outputs(5, 9, 7);
    rig.feed(5, 2);
    REQUIRE(rig.sectors_in(9) == 2u);
    rig.cmd(0x6100, 0, 9 << 8, 1);                      // GetSectorData: transferência em curso
    REQUIRE(rig.cd.data_port().read(0, 4) != 0u);

    rig.clear_hirq();
    const auto reply = rig.cmd(0x0401);
    CHECK(reply[0] != kRejected);
    const uint32_t expected = kMped | kEfls | kEcpy | kEhst | kEsel | 1u;
    CHECK((rig.hirq() & expected) == expected);

    CHECK(rig.sectors_in(9) == 0u);
    CHECK(rig.connection() == 0xFFu);
    CHECK(rig.cd.data_port().read(0, 4) == 0u);          // transferência descartada
    CHECK(rig.cmd(0x4300, 0, 5 << 8) == BootRig::Reply{0x0100, 0, 0, 0});
    CHECK(rig.cmd(0x7200)[1] == 2u);                     // diretório voltou à raiz (DIR + A.TXT)

    rig.feed(5);                                         // saída verdadeira de volta à partição 5
    CHECK(rig.sectors_in(5) == 1u);
    CHECK(rig.sectors_in(9) == 0u);
}

TEST_CASE("Init sem o bit 0 só devolve o status: mantém buffer, conexão e filtros, e não liga os bits de fim") {
    BootRig rig("cdb-init-plain");
    rig.cmd(0x4203, 0x1122, (5 << 8) | 0x07, 0x3344);
    rig.feed(5, 2);

    rig.clear_hirq();
    const auto reply = rig.cmd(0x0400);
    CHECK(reply[0] == 0x0100u);                          // PAUSE
    CHECK(reply[3] == kFadOffset);
    CHECK(rig.hirq() == 1u);                             // só CMOK
    CHECK(rig.sectors_in(5) == 2u);
    CHECK(rig.connection() == 5u);
    CHECK(rig.cmd(0x4300, 0, 5 << 8)[2] == 0x0007u);
}

TEST_CASE("Init interrompe um Play pendente e volta à posição inicial") {
    BootRig rig("cdb-init-play", 300 * 2048);
    rig.connect(0);
    rig.play(kFadOffset, 300);                                  // enche o buffer (200 setores) e para
    REQUIRE(rig.sectors_in(0) == 200u);
    rig.cmd(0x0400);
    CHECK(rig.cmd(0x0000)[3] == kFadOffset);
    rig.cmd(0x6200, 0, 0, 0xFFFF);                       // libera espaço: o Play não deve retomar
    CHECK(rig.sectors_in(0) == 0u);
}

// ---- Abort File

TEST_CASE("Abort File sinaliza EFLS e cancela a leitura de arquivo em andamento") {
    BootRig rig("cdb-abort", 300 * 2048);
    rig.cmd(0x7000, 0, 0x0000, 2);                       // DIR
    rig.cmd(0x3000, 0, 0x0000, 0);
    rig.cmd(0x7400, 0, 0x0000, 2);                       // ReadFile B.BIN (300 setores): trava no buffer cheio
    REQUIRE(rig.sectors_in(0) == 200u);

    rig.clear_hirq();
    const auto reply = rig.cmd(0x7500);
    CHECK(reply[0] != kRejected);
    CHECK((rig.hirq() & kEfls) != 0u);
    CHECK(reply[0] >> 8 == 0x01);                        // PAUSE

    rig.cmd(0x6200, 0, 0, 0xFFFF);
    CHECK(rig.sectors_in(0) == 0u);                      // a leitura não continuou
}

// ---- Get File Info com extensão XA

TEST_CASE("Get File Info lê unit_size, gap_size, nº de arquivo XA e atributos do registro ISO9660") {
    const std::vector<XaFile> xa = {
        {"X.BIN;1", 3, 4, 0x55, 7},          // atributos: 0x55 & 0xF8 = 0x50
        {"Y.BIN;1", 5, 6, 0xFF, 9, 'X', 'B'}  // sem "XA": a área é ignorada
    };
    BootRig rig("cdb-xa", 3000, xa);
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);
    REQUIRE(rig.cmd(0x7200)[1] == 4u);                   // DIR, A.TXT, X.BIN, Y.BIN

    auto info = [&](uint16_t id) {
        REQUIRE(rig.cmd(0x7300, 0, 0, id)[0] != kRejected);
        auto bytes = rig.read_port(12);
        rig.cmd(0x0600);
        return bytes;
    };
    const std::string x = info(4), y = info(5), a = info(3);
    CHECK(uint8_t(x[8]) == 3u);     // unit_size
    CHECK(uint8_t(x[9]) == 4u);     // gap_size
    CHECK(uint8_t(x[10]) == 7u);    // nº de arquivo XA
    CHECK(uint8_t(x[11]) == 0x50u); // atributos XA
    CHECK(uint8_t(y[8]) == 5u);
    CHECK(uint8_t(y[9]) == 6u);
    CHECK(uint8_t(y[10]) == 0u);    // "XB": sem extensão XA
    CHECK(uint8_t(y[11]) == 0u);
    CHECK(uint8_t(a[8]) == 0u);
    CHECK(uint8_t(a[10]) == 0u);
}

TEST_CASE("Get File Info 0xFFFFFF devolve 12 bytes por arquivo visível; ID fora da janela ou inexistente é rejeitado") {
    BootRig rig("cdb-xa-all", 3000, {{"X.BIN;1", 3, 4, 0x55, 7}});
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);
    const auto all = rig.cmd(0x7300, 0, 0x00FF, 0xFFFF);
    CHECK(all[1] == 18u);                                // 3 arquivos (DIR, A.TXT, X.BIN) x 6 palavras
    const std::string bytes = rig.read_port(36);
    CHECK(uint8_t(bytes[24 + 10]) == 7u);                // o 3º registro é o X.BIN
    rig.cmd(0x0600);
    CHECK(rig.cmd(0x7300, 0, 0, 99)[0] == kRejected);
}

// ---- Save/load

TEST_CASE("save/load do CD block preserva o tipo de autenticação, filtros, conexão e buffer") {
    BootRig rig("cdb-state");
    rig.cmd(0xE000, 0);
    rig.cmd(0x4203, 0x1122, (5 << 8) | 0x07, 0x3344);
    rig.filter_outputs(5, 9, 7);
    rig.feed(5, 2);
    saturn::StateWriter w;
    rig.cd.save(w);
    const auto bytes = w.take();

    BootRig fresh("cdb-state2");
    REQUIRE(fresh.cmd(0xE100)[1] == 0u);
    saturn::StateReader r(bytes);
    fresh.cd.load(r);

    CHECK(fresh.cmd(0xE100)[1] == 4u);
    CHECK(fresh.connection() == 5u);
    CHECK(fresh.cmd(0x4300, 0, 5 << 8) == BootRig::Reply{0x0103, 0x3344, 0x0007, 0x1122});
    CHECK(fresh.sectors_in(9) == 2u);
    fresh.cmd(0x6100, 0, 9 << 8, 1);
    CHECK(fresh.read_port(16) == "SEGA SEGASATURN ");   // conteúdo do setor vai no estado
    // a saída verdadeira (9) restaurada: novos setores continuam indo para a partição 9
    fresh.feed(5, 1);
    CHECK(fresh.sectors_in(9) == 3u);
}

TEST_CASE("save/load do CD block preserva a autenticação do tipo 2") {
    BootRig rig("cdb-state-t2", 3000, {}, false);
    rig.cmd(0xE000, 0);
    saturn::StateWriter w;
    rig.cd.save(w);
    const auto bytes = w.take();

    BootRig fresh("cdb-state-t2b");
    saturn::StateReader r(bytes);
    fresh.cd.load(r);
    CHECK(fresh.cmd(0xE100)[1] == 2u);
}

TEST_CASE("save/load do CD block preserva os campos XA do diretório carregado") {
    const std::vector<XaFile> xa = {{"X.BIN;1", 3, 4, 0x55, 7}};
    BootRig rig("cdb-state-xa", 3000, xa);
    rig.cmd(0x7000, 0, 0x00FF, 0xFFFF);
    saturn::StateWriter w;
    rig.cd.save(w);
    const auto bytes = w.take();

    BootRig fresh("cdb-state-xa2", 3000, xa);
    saturn::StateReader r(bytes);
    fresh.cd.load(r);
    REQUIRE(fresh.cmd(0x7300, 0, 0, 4)[0] != kRejected);   // X.BIN: DIR, A.TXT, X.BIN
    const std::string info = fresh.read_port(12);
    fresh.cmd(0x0600);
    CHECK(uint8_t(info[8]) == 3u);
    CHECK(uint8_t(info[9]) == 4u);
    CHECK(uint8_t(info[10]) == 7u);
    CHECK(uint8_t(info[11]) == 0x50u);
}

TEST_CASE("load do CD block recusa conexão, partição e posição de transferência fora dos limites") {
    BootRig rig("cdb-state-bad");
    rig.cmd(0x4203, 0, (5 << 8) | 0x07, 0);   // um filtro com saída verdadeira 5
    rig.filter_outputs(5, 9, 7);
    rig.feed(5, 1);                            // um setor na partição 9
    saturn::StateWriter w;
    rig.cd.save(w);
    const std::vector<uint8_t> good = w.take();

    const auto load = [&](const std::vector<uint8_t>& bytes) {
        BootRig target("cdb-state-bad2");
        saturn::StateReader r(bytes);
        target.cd.load(r);
    };
    CHECK_NOTHROW(load(good));

    // Campos fixos do início: hirq(2) mask(2) cr(4x2) status(1) auth(1) fad(4) play(4) words(4) subcode(8) since(8) → conexão (i32).
    constexpr size_t kConnectionOffset = 2 + 2 + 8 + 1 + 1 + 4 + 4 + 4 + 8 + 8;
    const auto with_i32 = [&](size_t offset, int32_t value) {
        saturn::StateWriter field;
        field.i32(value);
        const auto raw = field.take();
        std::vector<uint8_t> bytes = good;
        std::copy(raw.begin(), raw.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return bytes;
    };
    CHECK_NOTHROW(load(with_i32(kConnectionOffset, 0xFF)));             // sem conexão
    CHECK_NOTHROW(load(with_i32(kConnectionOffset, 23)));
    CHECK_THROWS_AS(load(with_i32(kConnectionOffset, 24)), saturn::StateError);
    CHECK_THROWS_AS(load(with_i32(kConnectionOffset, -1)), saturn::StateError);
    CHECK_THROWS_AS(load(with_i32(kConnectionOffset, 0x7FFFFFFF)), saturn::StateError);
}
