#include <catch2/catch_test_macros.hpp>

#include "saturnfs/image.hpp"
#include "support.hpp"

using namespace testsupport;

namespace {
std::filesystem::path make_disc(const std::string& tag, const std::vector<std::vector<uint8_t>>& sectors) {
    return write_single_track_disc(make_temp_dir(tag), sectors);
}
}  // namespace

TEST_CASE("read_user_sector devolve os 2048 bytes de usuário do setor pedido") {
    auto cue = make_disc("img1", {{0xAA}, {0xBB, 0xCC}});
    auto disc = saturnfs::DiscImage::open(cue);

    auto s = disc.read_user_sector(1);

    CHECK(s[0] == 0xBB);
    CHECK(s[1] == 0xCC);
    CHECK(s[2047] == 0x00);
}

TEST_CASE("read_user_sector lança para setor além do fim do .bin") {
    auto cue = make_disc("img2", {{1}, {2}});
    auto disc = saturnfs::DiscImage::open(cue);

    REQUIRE_THROWS(disc.read_user_sector(2));
}

TEST_CASE("open lança para .bin truncado no meio de um setor") {
    auto cue = make_disc("img3", {{1}, {2}});
    std::filesystem::resize_file(cue.parent_path() / "d.bin", saturnfs::kRawSectorSize + 100);

    REQUIRE_THROWS(saturnfs::DiscImage::open(cue));
}
