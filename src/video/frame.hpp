#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace video {

// Quadro de vídeo em RGBA de 8 bits por canal (um uint32_t por pixel: 0xAABBGGRR em little-endian, ou seja, bytes R, G, B, A).
struct Frame {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;

    Frame() = default;
    Frame(int w, int h) : width(w), height(h), pixels(size_t(w) * h, 0) {}
    uint32_t& at(int x, int y) { return pixels[size_t(y) * width + x]; }
    uint32_t at(int x, int y) const { return pixels[size_t(y) * width + x]; }
};

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

// PNG sem compressão (blocos "stored" do deflate): arquivo grande, mas sem dependência de zlib. Serve a diagnósticos e testes.
void write_png(const std::filesystem::path& path, const Frame& frame);

}  // namespace video
