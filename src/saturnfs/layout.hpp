#pragma once
#include <cstddef>

namespace saturnfs {

// Geometria de um setor MODE1/2352.
constexpr size_t kRawSectorSize = 2352;
constexpr size_t kUserSectorSize = 2048;
constexpr size_t kUserDataOffset = 16;

}  // namespace saturnfs
