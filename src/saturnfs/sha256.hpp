#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace saturnfs {

// SHA-256 incremental: update() quantas vezes quiser e digest()/hex() no fim (não alteram o estado).
class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;
    void update(std::span<const uint8_t> data);
    Digest digest() const;
    std::string hex() const;

private:
    std::array<uint32_t, 8> h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> block_{};
    size_t used_ = 0;      // bytes pendentes em block_
    uint64_t length_ = 0;  // total de bytes recebidos
};

std::string sha256_hex(std::span<const uint8_t> data);
std::string to_hex(std::span<const uint8_t> bytes);

}  // namespace saturnfs
