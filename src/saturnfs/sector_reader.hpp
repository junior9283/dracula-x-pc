#pragma once
#include <array>
#include <cstdint>

#include "saturnfs/layout.hpp"

namespace saturnfs {

// Fonte de setores de dados (2048 bytes de usuário por LBA): o disco do desenvolvedor hoje, o pacote local depois.
class SectorReader {
public:
    virtual ~SectorReader() = default;
    // Lança se `lba` estiver fora da fonte.
    virtual std::array<uint8_t, kUserSectorSize> read_user_sector(uint32_t lba) const = 0;
    virtual uint32_t sector_count() const = 0;
};

}  // namespace saturnfs
