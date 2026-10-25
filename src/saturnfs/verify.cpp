#include "saturnfs/verify.hpp"

#include <stdexcept>
#include <string_view>

namespace saturnfs {
namespace {

constexpr std::string_view kMagic = "SEGA SEGASATURN ";
constexpr std::string_view kSupportedProduct = "T-9527G";
constexpr std::string_view kSupportedVersion = "V1.400";

std::string field(const std::array<uint8_t, kUserSectorSize>& s, size_t offset, size_t width) {
    std::string text(reinterpret_cast<const char*>(s.data() + offset), width);
    text.erase(text.find_last_not_of(" \0", std::string::npos, 2) + 1);
    return text;
}

}  // namespace

SaturnHeader read_header(const SectorReader& source) {
    auto sector = source.read_user_sector(0);
    if (std::string_view(reinterpret_cast<const char*>(sector.data()), kMagic.size()) != kMagic)
        throw std::runtime_error("não é um disco de Sega Saturn (cabeçalho ausente)");
    return {field(sector, 0x10, 16), field(sector, 0x20, 10), field(sector, 0x2A, 6), field(sector, 0x30, 8),
            field(sector, 0x60, 112)};
}

void require_supported_disc(const SaturnHeader& header) {
    if (header.product_id != kSupportedProduct || header.version != kSupportedVersion)
        throw std::runtime_error("disco não suportado: " + header.product_id + " " + header.version +
                                 " (esperado " + std::string(kSupportedProduct) + " " + std::string(kSupportedVersion) + ")");
}

void require_complete_image(const SectorReader& source, uint32_t expected_sectors) {
    if (source.sector_count() != expected_sectors)
        throw std::runtime_error("imagem incompleta: a faixa de dados tem " + std::to_string(source.sector_count()) +
                                 " setores, esperado " + std::to_string(expected_sectors));
}

}  // namespace saturnfs
