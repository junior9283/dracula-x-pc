#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace saturnfs {

struct Track {
    enum class Kind { Data, Audio };
    int number;
    Kind kind;
    std::filesystem::path file;
    uint32_t start_lba;
};

// Lê um .cue e devolve as faixas. Lança std::runtime_error se algum .bin referenciado não existir.
std::vector<Track> parse_cue(const std::filesystem::path& cue);

}  // namespace saturnfs
