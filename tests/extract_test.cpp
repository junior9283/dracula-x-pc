#include <catch2/catch_test_macros.hpp>

#include "iso_builder.hpp"
#include "saturnfs/extract.hpp"

using namespace testsupport;
namespace fs = std::filesystem;

namespace {
saturnfs::DiscImage synthetic_disc(const std::string& tag) {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa").add("DIR/B.BIN", "beta").add("../EVIL", "x");
    return open_disc(iso, tag + "-disc");
}
}  // namespace

TEST_CASE("extract_all cria os arquivos, o manifest e não escapa da pasta de destino") {
    auto disc = synthetic_disc("ext1");
    auto out = make_temp_dir("ext1-out") / "disc";

    auto count = saturnfs::extract_all(disc, out, false);

    CHECK(count == 2);
    CHECK(read_text(out / "A.TXT") == "alfa");
    CHECK(read_text(out / "DIR/B.BIN") == "beta");
    CHECK_FALSE(fs::exists(out.parent_path() / "EVIL"));
    CHECK(read_text(out / "manifest.txt").find("A.TXT") != std::string::npos);
}

TEST_CASE("extract_all não sobrescreve pasta com conteúdo sem force") {
    auto disc = synthetic_disc("ext2");
    auto out = make_temp_dir("ext2-out") / "disc";
    saturnfs::extract_all(disc, out, false);

    REQUIRE_THROWS(saturnfs::extract_all(disc, out, false));
    REQUIRE_NOTHROW(saturnfs::extract_all(disc, out, true));
}

TEST_CASE("extract_all pula arquivo cujo extent fica fora da faixa de dados (ex.: CD3 sobre o áudio)") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa").add("Z.BIN", "no-audio");
    auto cue = iso.write(make_temp_dir("ext3-disc"));
    auto bin = cue.parent_path() / "d.bin";
    fs::resize_file(bin, fs::file_size(bin) - saturnfs::kRawSectorSize);  // Z.BIN passa a apontar para fora
    auto disc = saturnfs::DiscImage::open(cue);
    auto out = make_temp_dir("ext3-out") / "disc";

    auto count = saturnfs::extract_all(disc, out, false);

    CHECK(count == 1);
    CHECK(read_text(out / "A.TXT") == "alfa");
    CHECK_FALSE(fs::exists(out / "Z.BIN"));
    CHECK(read_text(out / "manifest.txt").find("# ignorado (fora da faixa de dados): Z.BIN") != std::string::npos);
}
