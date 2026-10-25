#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <vector>

#include "saturnfs/overlay.hpp"

using saturnfs::OverlayInfo;

namespace {
void put32(std::vector<uint8_t>& v, size_t off, uint32_t value) {
    for (int i = 0; i < 4; ++i) v[off + i] = (value >> (8 * (3 - i))) & 0xFF;
}

// Overlay sintético: `n` ponteiros válidos no início, uma palavra zero e depois dados.
std::vector<uint8_t> synthetic(uint32_t base, size_t total, size_t n) {
    std::vector<uint8_t> bytes(total, 0);
    for (size_t i = 0; i < n; ++i) put32(bytes, 4 * i, base + 0x40 + 4 * i);
    return bytes;
}
}  // namespace

TEST_CASE("known_overlay_base: grupos de memória conhecidos") {
    CHECK(saturnfs::known_overlay_base("STAGE_00.PRG") == 0x060DC000u);
    CHECK(saturnfs::known_overlay_base("boss_r13.prg") == 0x060DC000u);
    CHECK(saturnfs::known_overlay_base("TITLE.PRG") == 0x060A5000u);
    CHECK(saturnfs::known_overlay_base("ALUCARD.PRG") == 0x060A5000u);
    CHECK(saturnfs::known_overlay_base("GAME.PRG") == 0x06066000u);
    CHECK(saturnfs::known_overlay_base("MOVIE.PRG") == 0x06066000u);
    CHECK(saturnfs::known_overlay_base("T_BAT.PRG") == 0x060CF000u);
    CHECK(saturnfs::known_overlay_base("SUB.PRG") == 0x060D6000u);
    CHECK_FALSE(saturnfs::known_overlay_base("WEAPON0.PRG").has_value());
    CHECK_FALSE(saturnfs::known_overlay_base("QUALQUER.PRG").has_value());
    CHECK_FALSE(saturnfs::known_overlay_base("").has_value());
}

TEST_CASE("parse_overlay lê a tabela de ponteiros do cabeçalho e o endereço de carga") {
    auto bytes = synthetic(0x060DC000, 0x200, 10);

    auto info = saturnfs::parse_overlay("STAGE_00.PRG", bytes);

    CHECK(info.name == "STAGE_00.PRG");
    CHECK(info.load_address == 0x060DC000u);
    CHECK(info.size == 0x200u);
    REQUIRE(info.header_table.size() == 10);
    CHECK(info.header_table[0] == 0x060DC040u);
}

TEST_CASE("parse_overlay: ponteiro fora da região do próprio overlay encerra a tabela") {
    auto bytes = synthetic(0x060DC000, 0x200, 3);
    put32(bytes, 12, 0x060F0000);  // dentro da RAM, mas além do fim do overlay

    CHECK(saturnfs::parse_overlay("STAGE_00.PRG", bytes).header_table.size() == 3);
}

TEST_CASE("parse_overlay rejeita arquivo menor que uma palavra e nome sem base conhecida") {
    CHECK_THROWS_WITH(saturnfs::parse_overlay("STAGE_00.PRG", std::vector<uint8_t>(3)),
                      Catch::Matchers::ContainsSubstring("STAGE_00.PRG"));
    CHECK_THROWS_WITH(saturnfs::parse_overlay("WEAPON0.PRG", std::vector<uint8_t>(64)),
                      Catch::Matchers::ContainsSubstring("WEAPON0.PRG"));
}

TEST_CASE("find_overlay_conflicts: só sobreposição parcial entre bases diferentes conta") {
    OverlayInfo a{"A.PRG", 0x1000, 0x400, {}};
    OverlayInfo same_slot{"B.PRG", 0x1000, 0x200, {}};  // mesma base: alternativa do mesmo slot
    OverlayInfo partial{"C.PRG", 0x1200, 0x400, {}};     // sobrepõe A com base diferente
    OverlayInfo apart{"D.PRG", 0x9000, 0x100, {}};

    auto conflicts = saturnfs::find_overlay_conflicts(std::vector<OverlayInfo>{a, same_slot, partial, apart});

    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0] == "A.PRG x C.PRG");
}
