#include "saturnfs/package.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "saturnfs/sha256.hpp"

namespace saturnfs {
namespace {

constexpr char kMagic[8] = {'D', 'X', 'P', 'C', 'P', 'A', 'K', '1'};
constexpr uint32_t kVersion = 1, kHeaderSize = 64, kDigestOffset = 16;
constexpr uint32_t kBatchSectors = 512;   // 1 MiB por escrita

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }
uint32_t get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void write_bytes(std::ofstream& out, std::span<const uint8_t> bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

}  // namespace

std::string write_package(const SectorReader& source, const std::filesystem::path& path, const PackageProgress& progress,
                          std::string_view expected_digest) {
    const auto temp = std::filesystem::path(path) += ".tmp";
    Sha256 sha;
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("não foi possível criar o pacote em " + temp.string());
        std::array<uint8_t, kHeaderSize> header{};
        write_bytes(out, header);   // reservado; reescrito no fim com o resumo

        const uint32_t total = source.sector_count();
        std::vector<uint8_t> batch;
        batch.reserve(size_t(kBatchSectors) * kUserSectorSize);
        for (uint32_t lba = 0; lba < total; ++lba) {
            const auto sector = source.read_user_sector(lba);
            batch.insert(batch.end(), sector.begin(), sector.end());
            if (batch.size() == batch.capacity() || lba + 1 == total) {
                sha.update(batch);
                write_bytes(out, batch);
                batch.clear();
                if (progress) progress(lba + 1, total);
            }
        }
        if (!expected_digest.empty() && sha.hex() != expected_digest)
            throw std::runtime_error("o conteúdo do disco não confere com a versão suportada (imagem modificada ou com erro de leitura)");

        std::memcpy(header.data(), kMagic, sizeof kMagic);
        put32(header.data() + 8, kVersion);
        put32(header.data() + 12, total);
        const auto digest = sha.digest();
        std::memcpy(header.data() + kDigestOffset, digest.data(), digest.size());
        out.seekp(0);
        write_bytes(out, header);
        out.close();
        if (!out) throw std::runtime_error("falha ao gravar o pacote em " + temp.string());
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw;
    }
    std::filesystem::rename(temp, path);
    return sha.hex();
}

PackageReader PackageReader::open(const std::filesystem::path& path) {
    PackageReader r;
    r.buffer_ = std::make_unique<char[]>(kReadBuffer);
    r.file_.rdbuf()->pubsetbuf(r.buffer_.get(), kReadBuffer);
    r.file_.open(path, std::ios::binary);
    if (!r.file_) throw std::runtime_error("pacote não encontrado: " + path.string());
    std::array<uint8_t, kHeaderSize> header{};
    if (!r.file_.read(reinterpret_cast<char*>(header.data()), header.size()) || std::memcmp(header.data(), kMagic, sizeof kMagic) != 0)
        throw std::runtime_error("não é um pacote do port (DXPCPAK1): " + path.string());
    if (get32(header.data() + 8) != kVersion) throw std::runtime_error("versão de pacote não suportada: " + path.string());
    r.sectors_ = get32(header.data() + 12);
    r.digest_ = to_hex(std::span(header).subspan(kDigestOffset, 32));
    if (std::filesystem::file_size(path) != kHeaderSize + uint64_t(r.sectors_) * kUserSectorSize)
        throw std::runtime_error("pacote incompleto (gere de novo a partir do disco): " + path.string());
    return r;
}

std::array<uint8_t, kUserSectorSize> PackageReader::read_user_sector(uint32_t lba) const {
    if (lba >= sectors_) throw std::out_of_range("LBA fora do pacote: " + std::to_string(lba));
    std::array<uint8_t, kUserSectorSize> sector;
    if (lba != next_lba_) {   // leitura sequencial (o caso comum do CD) segue o buffer sem seek
        file_.clear();
        file_.seekg(std::streamoff(kHeaderSize) + std::streamoff(lba) * kUserSectorSize);
    }
    if (!file_.read(reinterpret_cast<char*>(sector.data()), sector.size())) {
        next_lba_ = ~0u;
        throw std::runtime_error("falha ao ler o pacote no LBA " + std::to_string(lba));
    }
    next_lba_ = lba + 1;
    return sector;
}

bool PackageReader::verify() const {
    Sha256 sha;
    for (uint32_t lba = 0; lba < sectors_; ++lba) sha.update(read_user_sector(lba));
    return sha.hex() == digest_;
}

}  // namespace saturnfs
