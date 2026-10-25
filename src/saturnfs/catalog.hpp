#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "saturnfs/image.hpp"
#include "saturnfs/iso9660.hpp"

namespace saturnfs {

enum class FileKind {
    MainExecutable, Overlay, Graphics, TileMap, Pattern, MusicStream, SoundBank, SoundRaw,
    Video, Bitmap, Font, Text, Unknown
};

// Classifica por nome (sem diferenciar maiúsculas); em SD/*.PCM o magic "DVI." decide entre música e som cru.
FileKind classify(std::string_view path, std::span<const uint8_t> head);

struct CatalogEntry {
    FileEntry file;
    FileKind kind;
};

// Catálogo de todos os arquivos do disco (lê só os primeiros bytes de cada um).
std::vector<CatalogEntry> build_catalog(const DiscImage& disc);

const char* kind_name(FileKind kind);

}  // namespace saturnfs
