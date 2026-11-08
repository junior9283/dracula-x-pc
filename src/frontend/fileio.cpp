#include "frontend/fileio.hpp"

#include <fstream>
#include <stdexcept>

namespace frontend {

std::vector<uint8_t> read_file_bytes(const std::filesystem::path& file) {
    if (auto bytes = try_read_file_bytes(file)) return std::move(*bytes);
    throw std::runtime_error("não foi possível ler " + file.string());
}

std::optional<std::vector<uint8_t>> try_read_file_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    std::vector<uint8_t> bytes(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return std::nullopt;
    return bytes;
}

void write_file_atomic(const std::filesystem::path& file, std::span<const uint8_t> bytes) {
    std::error_code error;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    auto temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) throw std::runtime_error("não foi possível gravar " + temp.string());
    }
    std::filesystem::rename(temp, file, error);
    if (error) throw std::runtime_error("não foi possível gravar " + file.string() + ": " + error.message());
}

}  // namespace frontend
