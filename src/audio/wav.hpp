#pragma once
#include <cstdint>
#include <filesystem>
#include <span>

namespace audio {

// Grava PCM 16 bits intercalado em um WAV canônico (RIFF/WAVE, PCM). Lança std::runtime_error se não conseguir gravar.
void write_wav_pcm16(const std::filesystem::path& file, std::span<const int16_t> samples, int sample_rate, int channels);

}  // namespace audio
