#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "saturnfs/cue.hpp"
#include "saturnfs/layout.hpp"

namespace saturnfs {

// Imagem de disco cue/bin. Os setores lidos vêm da primeira faixa de dados (MODE1/2352).
// Mantém o .bin aberto; não é seguro usar de várias threads ao mesmo tempo.
class DiscImage {
public:
    static DiscImage open(const std::filesystem::path& cue);

    std::array<uint8_t, kUserSectorSize> read_user_sector(uint32_t lba) const;
    std::span<const Track> tracks() const { return tracks_; }
    uint32_t data_sector_count() const { return data_sectors_; }

private:
    std::vector<Track> tracks_;
    mutable std::ifstream data_;
    uint32_t data_sectors_ = 0;
};

}  // namespace saturnfs
