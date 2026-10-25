#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <vector>

#include "audio/dvi.hpp"

using audio::DviHeader;

namespace {
void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = (x >> (8 * (3 - i))) & 0xFF;
}

// Arquivo DVI sintético: cabeçalho de 0x800 bytes + corpo.
std::vector<uint8_t> dvi_file(const std::vector<uint8_t>& body, uint32_t body_size_field, int16_t pred_l = 0, uint32_t idx_l = 0,
                              int16_t pred_r = 100, uint32_t idx_r = 0, uint32_t loop = 0x1000, uint32_t header_size = 0x800) {
    std::vector<uint8_t> f(0x800 + body.size(), 0);
    f[0] = 'D'; f[1] = 'V'; f[2] = 'I'; f[3] = '.';
    put32(f, 4, header_size);
    put32(f, 8, body_size_field);
    put32(f, 0xC, loop);
    put32(f, 0x10, uint32_t(uint16_t(pred_l)) << 16 | 0x7FFF);
    put32(f, 0x14, idx_l);
    put32(f, 0x20, uint32_t(uint16_t(pred_r)) << 16);
    put32(f, 0x24, idx_r);
    std::copy(body.begin(), body.end(), f.begin() + 0x800);
    return f;
}
}  // namespace

TEST_CASE("parse_dvi_header lê tamanhos, laço e estado inicial dos dois canais") {
    auto file = dvi_file(std::vector<uint8_t>(16), 16, -1570, 47, -2219, 35);

    DviHeader h = audio::parse_dvi_header(file);

    CHECK(h.header_size == 0x800);
    CHECK(h.body_size == 16);
    CHECK(h.loop_start_frames == 0x1000u);
    CHECK(h.channel[0].predictor == -1570);
    CHECK(h.channel[0].index == 47);
    CHECK(h.channel[1].predictor == -2219);
    CHECK(h.channel[1].index == 35);
}

TEST_CASE("parse_dvi_header rejeita arquivo curto, magic errado, tamanho inconsistente e índice > 88") {
    CHECK_THROWS(audio::parse_dvi_header(std::vector<uint8_t>(100)));

    auto bad_magic = dvi_file(std::vector<uint8_t>(8), 8);
    bad_magic[0] = 'X';
    CHECK_THROWS_WITH(audio::parse_dvi_header(bad_magic), Catch::Matchers::ContainsSubstring("DVI"));

    CHECK_THROWS_WITH(audio::parse_dvi_header(dvi_file(std::vector<uint8_t>(8), 9)), Catch::Matchers::ContainsSubstring("tamanho"));
    CHECK_THROWS_WITH(audio::parse_dvi_header(dvi_file(std::vector<uint8_t>(8), 8, 0, 89)), Catch::Matchers::ContainsSubstring("índice"));
}

TEST_CASE("decode_dvi: IMA estéreo em grupos de 4 bytes por canal, nibble alto primeiro") {
    // Canal L: bytes 0x77, 0xF0, 0, 0 -> nibbles (alto primeiro) 7,7,F,0,0,0,0,0 a partir de (pred 0, índice 0): 11, 41, -22, -13...
    // Canal R: tudo zero a partir de (pred 100, índice 0): permanece 100.
    std::vector<uint8_t> body = {0x77, 0xF0, 0, 0, 0, 0, 0, 0};
    auto file = dvi_file(body, 8);
    DviHeader h = audio::parse_dvi_header(file);

    auto pcm = audio::decode_dvi(std::span<const uint8_t>(file).subspan(0x800), h);

    REQUIRE(pcm.size() == 16);  // 8 quadros estéreo intercalados L,R
    CHECK(pcm[0] == 11);   CHECK(pcm[1] == 100);
    CHECK(pcm[2] == 41);   CHECK(pcm[3] == 100);
    CHECK(pcm[4] == -22);  CHECK(pcm[5] == 100);
    CHECK(pcm[6] == -13);  CHECK(pcm[7] == 100);
}

TEST_CASE("decode_dvi: corpo truncado decodifica só grupos completos de 8 bytes, sem ler além do fim") {
    std::vector<uint8_t> body(11, 0);  // 1 grupo completo + 3 bytes soltos
    auto file = dvi_file(body, 11);
    DviHeader h = audio::parse_dvi_header(file);

    auto pcm = audio::decode_dvi(std::span<const uint8_t>(file).subspan(0x800), h);

    CHECK(pcm.size() == 16);
}

TEST_CASE("parse_dvi_header: laço 0xFFFFFFFF significa sem laço (11 das 49 faixas reais)") {
    auto looped = audio::parse_dvi_header(dvi_file(std::vector<uint8_t>(8), 8));
    auto one_shot = audio::parse_dvi_header(dvi_file(std::vector<uint8_t>(8), 8, 0, 0, 100, 0, 0xFFFFFFFF));

    REQUIRE(looped.loop_start_frames.has_value());
    CHECK(*looped.loop_start_frames == 0x1000u);
    CHECK_FALSE(one_shot.loop_start_frames.has_value());
}

TEST_CASE("parse_dvi_header rejeita header_size diferente de 0x800 (o cabeçalho viraria áudio)") {
    // dvi_file monta 0x800 bytes de cabeçalho + corpo; com header_size=16 e body_size=0x7F0 a soma não bate com o arquivo,
    // então encurtamos para 16 + 0x7F0 bytes para isolar a regra do tamanho de cabeçalho.
    auto file = dvi_file(std::vector<uint8_t>(0x7F0), 0x7F0, 0, 0, 100, 0, 0x1000, 16);
    file.resize(16 + 0x7F0);

    CHECK_THROWS_WITH(audio::parse_dvi_header(file), Catch::Matchers::ContainsSubstring("cabeçalho"));
}
