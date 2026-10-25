#include "saturnfs/iso9660.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace saturnfs {
namespace {

constexpr uint32_t kPrimaryDescriptorLba = 16;
constexpr size_t kRootRecordOffset = 156;
constexpr size_t kMinRecordLength = 34;
constexpr size_t kNameLengthOffset = 32, kNameOffset = 33, kFlagsOffset = 25;
constexpr uint8_t kDirectoryFlag = 2;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

std::vector<uint8_t> read_extent(const SectorReader& source, uint32_t lba, uint32_t size) {
    std::vector<uint8_t> bytes;
    bytes.reserve(size);
    for (uint32_t offset = 0; offset < size; offset += kUserSectorSize) {
        auto sector = source.read_user_sector(lba + offset / kUserSectorSize);
        auto chunk = std::min<size_t>(kUserSectorSize, size - offset);
        bytes.insert(bytes.end(), sector.begin(), sector.begin() + chunk);
    }
    return bytes;
}

bool is_safe_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
}

DirEntry parse_record(const uint8_t* rec) {
    std::string name(reinterpret_cast<const char*>(rec + kNameOffset), rec[kNameLengthOffset]);
    if (name == std::string(1, '\0')) name = ".";
    else if (name == std::string(1, '\1')) name = "..";
    else if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
    return {std::move(name), le32(rec + 2), le32(rec + 10), (rec[kFlagsOffset] & kDirectoryFlag) != 0};
}

void walk(const SectorReader& source, const DirEntry& dir, const std::string& prefix, std::set<uint32_t>& visited,
          std::vector<FileEntry>& out) {
    if (!visited.insert(dir.lba).second) return;  // diretório que aponta para si/ancestral
    for (const DirEntry& entry : list_directory(source, dir.lba, dir.size)) {
        if (!is_safe_name(entry.name)) continue;  // inclui "." e ".."
        const std::string path = prefix + entry.name;
        if (entry.is_directory) walk(source, entry, path + "/", visited, out);
        else out.push_back({path, entry.lba, entry.size});
    }
}

}  // namespace

DirEntry root_directory(const SectorReader& source) {
    auto pvd = source.read_user_sector(kPrimaryDescriptorLba);
    if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0) throw std::runtime_error("descritor ISO9660 ausente");
    DirEntry root = parse_record(pvd.data() + kRootRecordOffset);
    root.name = "/";
    return root;
}

std::vector<DirEntry> list_directory(const SectorReader& source, uint32_t lba, uint32_t size) {
    const auto dir = read_extent(source, lba, size);
    std::vector<DirEntry> entries;
    size_t pos = 0;
    while (pos < dir.size()) {
        const size_t len = dir[pos];
        if (len == 0) {  // registros não cruzam setores: pula para o próximo
            pos = (pos / kUserSectorSize + 1) * kUserSectorSize;
            continue;
        }
        if (pos + len > dir.size() || len < kMinRecordLength || kNameOffset + size_t(dir[pos + kNameLengthOffset]) > len)
            throw std::runtime_error("registro de diretório inválido");
        entries.push_back(parse_record(dir.data() + pos));
        pos += len;
    }
    return entries;
}

std::vector<FileEntry> list_files(const SectorReader& source) {
    std::set<uint32_t> visited;
    std::vector<FileEntry> files;
    walk(source, root_directory(source), "", visited, files);
    std::ranges::sort(files, {}, &FileEntry::path);
    return files;
}

bool is_in_data_track(const SectorReader& source, const FileEntry& entry) {
    return uint64_t(entry.lba) + (uint64_t(entry.size) + kUserSectorSize - 1) / kUserSectorSize <= source.sector_count();
}

std::vector<uint8_t> read_file(const SectorReader& source, const FileEntry& entry) { return read_extent(source, entry.lba, entry.size); }

}  // namespace saturnfs
