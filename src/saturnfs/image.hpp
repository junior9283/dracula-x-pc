#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "saturnfs/cue.hpp"

namespace saturnfs {

constexpr size_t kUserSectorSize = 2048;
constexpr size_t kRawSectorSize = 2352;
constexpr size_t kUserDataOffset = 16;

// Imagem de disco cue/bin. Só a primeira faixa de dados (MODE1/2352) é lida por setor.
class DiscImage {
public:
    static DiscImage open(const std::filesystem::path& cue);

    std::array<uint8_t, kUserSectorSize> read_user_sector(uint32_t lba) const;
    std::span<const Track> tracks() const { return tracks_; }

private:
    std::vector<Track> tracks_;
    uint32_t data_sectors_ = 0;
};

}  // namespace saturnfs
