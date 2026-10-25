#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>

#include "iso_builder.hpp"
#include "saturnfs/iso9660.hpp"
#include "saturnfs/sha256.hpp"
#include "saturnfs/verify.hpp"

using namespace testsupport;

TEST_CASE("list_files percorre a raiz e subdiretórios sem o sufixo ;1") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "olá").add("DIR/B.BIN", "dados");
    auto disc = open_disc(iso, "iso1");

    auto files = saturnfs::list_files(disc);

    REQUIRE(files.size() == 2);
    CHECK(files[0].path == "A.TXT");
    CHECK(files[0].size == 4);  // "olá" em UTF-8
    CHECK(files[1].path == "DIR/B.BIN");
}

TEST_CASE("read_file devolve os bytes do arquivo, inclusive os que cruzam setores") {
    std::string big(5000, 'x');
    big[4999] = 'Z';
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("BIG.BIN", big);
    auto disc = open_disc(iso, "iso2");

    auto files = saturnfs::list_files(disc);
    auto bytes = saturnfs::read_file(disc, files.at(0));

    REQUIRE(bytes.size() == 5000);
    CHECK(bytes[4999] == 'Z');
}

TEST_CASE("list_files ignora entradas de nome '..' (sem escapar da pasta)") {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "a").add("../EVIL", "x");
    auto disc = open_disc(iso, "iso3");

    auto files = saturnfs::list_files(disc);

    REQUIRE(files.size() == 1);
    CHECK(files[0].path == "A.TXT");
}

TEST_CASE("read_header lê produto e versão do setor 0") {
    SyntheticIso iso("T-9527G", "V1.400");
    auto disc = open_disc(iso, "iso4");

    auto header = saturnfs::read_header(disc);

    CHECK(header.product_id == "T-9527G");
    CHECK(header.version == "V1.400");
    CHECK(header.release_date == "19980427");
}

TEST_CASE("require_supported_disc aceita T-9527G V1.400 e recusa outro, citando o ID") {
    saturnfs::SaturnHeader ok{"SEGA", "T-9527G", "V1.400", "19980427", "X"};
    saturnfs::SaturnHeader other{"SEGA", "T-0000X", "V1.000", "19990101", "Y"};

    REQUIRE_NOTHROW(saturnfs::require_supported_disc(ok));
    REQUIRE_THROWS_WITH(saturnfs::require_supported_disc(other), Catch::Matchers::ContainsSubstring("T-0000X"));
}

TEST_CASE("sha256_hex de 'abc' é o vetor conhecido") {
    std::string abc = "abc";
    std::vector<uint8_t> bytes(abc.begin(), abc.end());
    CHECK(saturnfs::sha256_hex(bytes) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
