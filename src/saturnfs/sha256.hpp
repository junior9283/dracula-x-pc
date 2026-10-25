#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace saturnfs {

std::string sha256_hex(std::span<const uint8_t> data);

}  // namespace saturnfs
