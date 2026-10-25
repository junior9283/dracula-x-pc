#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "saturnfs/catalog.hpp"

using saturnfs::FileKind;
using saturnfs::classify;

namespace {
std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
}  // namespace

TEST_CASE("classify: SD/*.PCM com magic DVI. é música em fluxo") {
    CHECK(classify("SD/SD01.PCM", bytes("DVI.\0\0\x08\0")) == FileKind::MusicStream);
}

TEST_CASE("classify: SD/*.PCM sem magic, ou com cabeçalho vazio, é som cru") {
    CHECK(classify("SD/SDE0.PCM", bytes(std::string(16, '\0'))) == FileKind::SoundRaw);
    CHECK(classify("SD/SD01.PCM", {}) == FileKind::SoundRaw);
}

TEST_CASE("classify: tipos por nome, sem diferenciar maiúsculas") {
    CHECK(classify("0.BIN", {}) == FileKind::MainExecutable);
    CHECK(classify("stage_00.prg", {}) == FileKind::Overlay);
    CHECK(classify("STAGE_00.CHR", {}) == FileKind::Graphics);
    CHECK(classify("STAGE_00.MAP", {}) == FileKind::TileMap);
    CHECK(classify("S011.PTN", {}) == FileKind::Pattern);
    CHECK(classify("SD/SD_ALK.CRT", {}) == FileKind::SoundBank);
    CHECK(classify("OPENING.AVI", {}) == FileKind::Video);
    CHECK(classify("OMAKE/24/DX01.BMP", {}) == FileKind::Bitmap);
    CHECK(classify("KANJI.FON", {}) == FileKind::Font);
    CHECK(classify("ABS.TXT", {}) == FileKind::Text);
    CHECK(classify("OMAKE/KAIHATSU.DOC", {}) == FileKind::Text);
}

TEST_CASE("classify: extensão desconhecida ou sem extensão é Unknown, sem exceção") {
    CHECK(classify("X.ZZZ", {}) == FileKind::Unknown);
    CHECK(classify("SEMEXT", {}) == FileKind::Unknown);
    CHECK(classify("EVENT020.001", {}) == FileKind::Unknown);
}

#include "iso_builder.hpp"

TEST_CASE("build_catalog classifica os arquivos e pula os que ficam fora da faixa de dados") {
    using namespace testsupport;
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa").add("Z.BIN", "no-audio");
    auto cue = iso.write(make_temp_dir("cat-ghost"));
    auto bin = cue.parent_path() / "d.bin";
    std::filesystem::resize_file(bin, std::filesystem::file_size(bin) - saturnfs::kRawSectorSize);
    auto disc = saturnfs::DiscImage::open(cue);

    auto catalog = saturnfs::build_catalog(disc);

    REQUIRE(catalog.size() == 1);
    CHECK(catalog[0].file.path == "A.TXT");
    CHECK(catalog[0].kind == FileKind::Text);
}
