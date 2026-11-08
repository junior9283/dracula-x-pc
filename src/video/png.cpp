#include <array>
#include <fstream>
#include <stdexcept>

#include "video/frame.hpp"

namespace video {
namespace {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

void write_chunk(std::ofstream& file, const char type[4], const std::vector<uint8_t>& data) {
    std::vector<uint8_t> chunk;
    put32(chunk, uint32_t(data.size()));
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), data.begin(), data.end());
    const uint32_t crc = crc32(chunk.data() + 4, chunk.size() - 4) ^ 0xFFFFFFFFu;
    put32(chunk, crc);
    file.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size()));
}

}  // namespace

void write_png(const std::filesystem::path& path, const Frame& frame) {
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("não foi possível criar " + path.string());
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    file.write(reinterpret_cast<const char*>(kSignature), 8);

    std::vector<uint8_t> header;
    put32(header, uint32_t(frame.width));
    put32(header, uint32_t(frame.height));
    header.insert(header.end(), {8, 2, 0, 0, 0});   // 8 bits, RGB, sem compressão extra, filtro 0, sem entrelaçamento
    write_chunk(file, "IHDR", header);

    std::vector<uint8_t> raw;                        // cada linha: byte de filtro 0 + RGB
    raw.reserve(size_t(frame.height) * (1 + size_t(frame.width) * 3));
    for (int y = 0; y < frame.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < frame.width; ++x) {
            const uint32_t p = frame.at(x, y);
            raw.push_back(uint8_t(p));
            raw.push_back(uint8_t(p >> 8));
            raw.push_back(uint8_t(p >> 16));
        }
    }

    std::vector<uint8_t> z = {0x78, 0x01};           // zlib: deflate, sem dicionário
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    put32(z, (b << 16) | a);
    write_chunk(file, "IDAT", z);
    write_chunk(file, "IEND", {});
}

}  // namespace video
