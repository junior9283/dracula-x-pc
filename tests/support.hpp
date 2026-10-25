#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

#include "saturnfs/layout.hpp"

namespace testsupport {

inline std::filesystem::path make_temp_dir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() / ("dxpc-test-" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Monta um setor MODE1/2352: sync, cabeçalho, saturnfs::kUserSectorSize bytes de usuário, EDC/ECC zerados.
inline std::array<uint8_t, saturnfs::kRawSectorSize> raw_sector(uint32_t lba, const std::vector<uint8_t>& user) {
    std::array<uint8_t, saturnfs::kRawSectorSize> s{};
    s[0] = 0x00;
    for (int i = 1; i <= 10; ++i) s[i] = 0xFF;
    s[11] = 0x00;
    s[12] = 0; s[13] = 2; s[14] = static_cast<uint8_t>(lba & 0xFF); s[15] = 1;
    std::memcpy(s.data() + saturnfs::kUserDataOffset, user.data(), std::min<size_t>(user.size(), saturnfs::kUserSectorSize));
    return s;
}

inline void write_bin(const std::filesystem::path& file, const std::vector<std::vector<uint8_t>>& sectors_user) {
    std::ofstream out(file, std::ios::binary);
    uint32_t lba = 0;
    for (const auto& u : sectors_user) {
        auto s = raw_sector(lba++, u);
        out.write(reinterpret_cast<const char*>(s.data()), s.size());
    }
}

inline void write_text(const std::filesystem::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary) << text;
}

inline std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Disco de uma faixa MODE1: grava d.bin + d.cue em `dir` e devolve o caminho do .cue.
inline std::filesystem::path write_single_track_disc(const std::filesystem::path& dir,
                                                     const std::vector<std::vector<uint8_t>>& sectors_user) {
    write_bin(dir / "d.bin", sectors_user);
    write_text(dir / "d.cue", "FILE \"d.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    return dir / "d.cue";
}

}  // namespace testsupport
