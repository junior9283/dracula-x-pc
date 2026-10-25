#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "saturnfs/cue.hpp"
#include "support.hpp"

using namespace testsupport;
using saturnfs::Track;

TEST_CASE("parse_cue lê duas faixas com nome contendo espaços e parênteses") {
    auto dir = make_temp_dir("cue1");
    write_bin(dir / "Jogo (Japan) (Track 1).bin", {{1}, {2}, {3}});
    write_text(dir / "Jogo (Japan) (Track 2).bin", std::string(saturnfs::kRawSectorSize * 2, '\0'));
    write_text(dir / "jogo.cue",
               "FILE \"Jogo (Japan) (Track 1).bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
               "FILE \"Jogo (Japan) (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n");

    auto tracks = saturnfs::parse_cue(dir / "jogo.cue");

    REQUIRE(tracks.size() == 2);
    CHECK(tracks[0].number == 1);
    CHECK(tracks[0].kind == Track::Kind::Data);
    CHECK(tracks[0].start_lba == 0);
    CHECK(tracks[1].number == 2);
    CHECK(tracks[1].kind == Track::Kind::Audio);
    CHECK(tracks[1].start_lba == 3 + 150);  // 3 setores da faixa 1 + INDEX 01 de 00:02:00
    CHECK(tracks[1].file == dir / "Jogo (Japan) (Track 2).bin");
}

TEST_CASE("parse_cue lança quando o .bin referenciado não existe") {
    auto dir = make_temp_dir("cue2");
    write_text(dir / "x.cue", "FILE \"nao-existe.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");

    REQUIRE_THROWS_WITH(saturnfs::parse_cue(dir / "x.cue"), Catch::Matchers::ContainsSubstring("nao-existe.bin"));
}
