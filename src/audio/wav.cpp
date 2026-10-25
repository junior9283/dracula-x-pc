#include "audio/wav.hpp"

#include <array>
#include <fstream>
#include <stdexcept>

namespace audio {
namespace {

void put_le(std::ofstream& out, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
}

}  // namespace

void write_wav_pcm16(const std::filesystem::path& file, std::span<const int16_t> samples, int sample_rate, int channels) {
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    std::ofstream out(file, std::ios::binary);
    out.write("RIFF", 4);
    put_le(out, 36 + data_bytes, 4);
    out.write("WAVEfmt ", 8);
    put_le(out, 16, 4);                                  // tamanho do bloco fmt
    put_le(out, 1, 2);                                   // PCM
    put_le(out, channels, 2);
    put_le(out, sample_rate, 4);
    put_le(out, sample_rate * channels * 2, 4);          // bytes por segundo
    put_le(out, channels * 2, 2);                        // alinhamento do bloco
    put_le(out, 16, 2);                                  // bits por amostra
    out.write("data", 4);
    put_le(out, data_bytes, 4);
    for (int16_t s : samples) put_le(out, static_cast<uint16_t>(s), 2);
    if (!out) throw std::runtime_error("falha ao gravar " + file.string());
}

}  // namespace audio
