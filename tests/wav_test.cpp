#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "audio/wav.hpp"
#include "support.hpp"

TEST_CASE("write_wav_pcm16 grava cabeçalho RIFF/WAVE estéreo de 16 bits e os dados") {
    auto dir = testsupport::make_temp_dir("wav1");
    std::vector<int16_t> pcm = {1, -2, 3, -4};  // 2 quadros estéreo

    audio::write_wav_pcm16(dir / "t.wav", pcm, 44100, 2);

    auto bytes = testsupport::read_text(dir / "t.wav");
    REQUIRE(bytes.size() == 44 + 8);
    CHECK(bytes.substr(0, 4) == "RIFF");
    CHECK(bytes.substr(8, 4) == "WAVE");
    CHECK(uint8_t(bytes[22]) == 2);                      // canais
    CHECK(*reinterpret_cast<const uint32_t*>(&bytes[24]) == 44100u);  // taxa
    CHECK(*reinterpret_cast<const uint32_t*>(&bytes[40]) == 8u);      // bytes de dados
    CHECK(*reinterpret_cast<const int16_t*>(&bytes[46]) == -2);       // segunda amostra
}
