#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "saturnfs/image.hpp"

namespace saturnfs {

struct FileEntry {
    std::string path;  // relativo, separador '/', sem ";1"
    uint32_t lba;
    uint32_t size;
};

// Lista os arquivos do ISO9660 (recursivo). Entradas com nome "." ou ".." são ignoradas.
std::vector<FileEntry> list_files(const DiscImage& disc);

std::vector<uint8_t> read_file(const DiscImage& disc, const FileEntry& entry);

}  // namespace saturnfs
