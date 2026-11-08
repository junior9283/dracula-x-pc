#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

#include "iso_builder.hpp"
#include "saturnfs/package.hpp"
#include "saturnfs/sha256.hpp"

using namespace testsupport;

TEST_CASE("SHA-256 incremental: pedaços de tamanhos variados dão o mesmo resumo que o cálculo de uma vez") {
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7 + 3);
    saturnfs::Sha256 h;
    h.update(std::span(data).first(1));
    h.update(std::span(data).subspan(1, 130));
    h.update(std::span(data).subspan(131));
    CHECK(h.hex() == saturnfs::sha256_hex(data));
    CHECK(saturnfs::sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("pacote local: os setores gravados são lidos de volta iguais, sem precisar do disco") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa");
    const auto disc = open_disc(iso, "pak1");
    const auto pak = make_temp_dir("pak1-out") / "jogo.pak";

    int calls = 0;
    const std::string digest = saturnfs::write_package(disc, pak, [&](uint32_t done, uint32_t total) { ++calls; CHECK(done <= total); });

    const auto reader = saturnfs::PackageReader::open(pak);
    REQUIRE(reader.sector_count() == disc.sector_count());
    for (uint32_t lba = 0; lba < disc.sector_count(); ++lba) CHECK(reader.read_user_sector(lba) == disc.read_user_sector(lba));
    CHECK(reader.digest() == digest);
    CHECK(reader.verify());
    CHECK(calls > 0);
    CHECK_THROWS(reader.read_user_sector(disc.sector_count()));
}

TEST_CASE("pacote local: arquivo sem a assinatura ou truncado é recusado com mensagem clara") {
    const auto dir = make_temp_dir("pak-bad");
    std::ofstream(dir / "lixo.pak") << "nada a ver";
    CHECK_THROWS_WITH(saturnfs::PackageReader::open(dir / "lixo.pak"), Catch::Matchers::ContainsSubstring("pacote"));

    SyntheticIso iso("T-9527G", "V1.400");
    const auto disc = open_disc(iso, "pak-trunc");
    const auto pak = dir / "curto.pak";
    saturnfs::write_package(disc, pak);
    std::filesystem::resize_file(pak, std::filesystem::file_size(pak) - 100);
    CHECK_THROWS_WITH(saturnfs::PackageReader::open(pak), Catch::Matchers::ContainsSubstring("incompleto"));
}

TEST_CASE("pacote local: resumo esperado diferente recusa a geração e não deixa arquivo") {
    SyntheticIso iso("T-9527G", "V1.400");
    const auto disc = open_disc(iso, "pak-digest");
    const auto pak = make_temp_dir("pak-digest-out") / "jogo.pak";
    const std::string real = saturnfs::write_package(disc, pak);
    std::filesystem::remove(pak);
    CHECK(saturnfs::write_package(disc, pak, {}, real) == real);
    std::filesystem::remove(pak);
    CHECK_THROWS_WITH(saturnfs::write_package(disc, pak, {}, std::string(64, '0')), Catch::Matchers::ContainsSubstring("não confere"));
    CHECK_FALSE(std::filesystem::exists(pak));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(pak) += ".tmp"));
}

TEST_CASE("pacote local: leitura fora de ordem depois de sequencial devolve o setor certo") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("B.TXT", std::string(5000, 'b'));
    const auto disc = open_disc(iso, "pak-seek");
    const auto pak = make_temp_dir("pak-seek-out") / "jogo.pak";
    saturnfs::write_package(disc, pak);
    const auto reader = saturnfs::PackageReader::open(pak);
    const uint32_t n = disc.sector_count();
    for (uint32_t lba : {0u, 1u, 2u, n - 1, 0u, n / 2, n / 2 + 1, 3u}) CHECK(reader.read_user_sector(lba) == disc.read_user_sector(lba));
}
