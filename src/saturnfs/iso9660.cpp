#include "saturnfs/iso9660.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace saturnfs {
namespace {

constexpr uint32_t kPrimaryDescriptorLba = 16;
constexpr size_t kRootRecordOffset = 156;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

std::vector<uint8_t> read_extent(const DiscImage& disc, uint32_t lba, uint32_t size) {
    std::vector<uint8_t> bytes;
    bytes.reserve(size);
    for (uint32_t offset = 0; offset < size; offset += kUserSectorSize) {
        auto sector = disc.read_user_sector(lba + offset / kUserSectorSize);
        auto chunk = std::min<size_t>(kUserSectorSize, size - offset);
        bytes.insert(bytes.end(), sector.begin(), sector.begin() + chunk);
    }
    return bytes;
}

bool is_safe_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
}

void walk(const DiscImage& disc, uint32_t lba, uint32_t size, const std::string& prefix,
          std::set<uint32_t>& visited, std::vector<FileEntry>& out) {
    if (!visited.insert(lba).second) return;  // diretório que aponta para si/ancestral
    auto dir = read_extent(disc, lba, size);

    size_t pos = 0;
    while (pos < dir.size()) {
        size_t len = dir[pos];
        if (len == 0) {  // registros não cruzam setores: pula para o próximo
            pos = (pos / kUserSectorSize + 1) * kUserSectorSize;
            continue;
        }
        if (pos + len > dir.size() || len < 34 || 33 + size_t(dir[pos + 32]) > len)
            throw std::runtime_error("registro de diretório inválido");

        const uint8_t* rec = dir.data() + pos;
        std::string name(reinterpret_cast<const char*>(rec + 33), rec[32]);
        if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
        bool is_dir = rec[25] & 2;
        pos += len;

        if (!is_safe_name(name)) continue;  // inclui '.'(0x00) e '..'(0x01) do ISO
        std::string path = prefix + name;
        if (is_dir) walk(disc, le32(rec + 2), le32(rec + 10), path + "/", visited, out);
        else out.push_back({path, le32(rec + 2), le32(rec + 10)});
    }
}

}  // namespace

std::vector<FileEntry> list_files(const DiscImage& disc) {
    auto pvd = disc.read_user_sector(kPrimaryDescriptorLba);
    if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) throw std::runtime_error("descritor ISO9660 ausente");

    const uint8_t* root = pvd.data() + kRootRecordOffset;
    std::set<uint32_t> visited;
    std::vector<FileEntry> files;
    walk(disc, le32(root + 2), le32(root + 10), "", visited, files);
    std::ranges::sort(files, {}, &FileEntry::path);
    return files;
}

std::vector<uint8_t> read_file(const DiscImage& disc, const FileEntry& entry) {
    return read_extent(disc, entry.lba, entry.size);
}

}  // namespace saturnfs
