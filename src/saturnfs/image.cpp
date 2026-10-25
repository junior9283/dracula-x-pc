#include "saturnfs/image.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace saturnfs {

DiscImage DiscImage::open(const std::filesystem::path& cue) {
    DiscImage disc;
    disc.tracks_ = parse_cue(cue);
    auto data = std::ranges::find(disc.tracks_, Track::Kind::Data, &Track::kind);
    if (data == disc.tracks_.end()) throw std::runtime_error("o cue não tem faixa de dados");

    auto size = std::filesystem::file_size(data->file);
    if (size % kRawSectorSize != 0) throw std::runtime_error("faixa de dados truncada: " + data->file.filename().string());
    disc.data_sectors_ = static_cast<uint32_t>(size / kRawSectorSize);
    return disc;
}

std::array<uint8_t, kUserSectorSize> DiscImage::read_user_sector(uint32_t lba) const {
    if (lba >= data_sectors_) throw std::out_of_range("setor fora da imagem: " + std::to_string(lba));
    std::ifstream in(tracks_.front().file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(lba) * kRawSectorSize + kUserDataOffset);
    std::array<uint8_t, kUserSectorSize> sector;
    in.read(reinterpret_cast<char*>(sector.data()), sector.size());
    if (!in) throw std::runtime_error("falha ao ler o setor " + std::to_string(lba));
    return sector;
}

}  // namespace saturnfs
