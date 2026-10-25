#include "saturnfs/catalog.hpp"

#include <algorithm>
#include <array>
#include <string>

#include "saturnfs/strutil.hpp"

namespace saturnfs {
namespace {

constexpr std::array<uint8_t, 4> kDviMagic = {'D', 'V', 'I', '.'};
constexpr size_t kHeadBytes = 16;

}  // namespace

FileKind classify(std::string_view path, std::span<const uint8_t> head) {
    const std::string name = to_lower(path);
    const auto dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);

    if (name == "0.bin") return FileKind::MainExecutable;
    if (ext == "pcm" && name.starts_with("sd/"))
        return head.size() >= kDviMagic.size() && std::equal(kDviMagic.begin(), kDviMagic.end(), head.begin())
                   ? FileKind::MusicStream
                   : FileKind::SoundRaw;
    if (ext == "crt") return FileKind::SoundBank;
    if (ext == "prg") return FileKind::Overlay;
    if (ext == "chr") return FileKind::Graphics;
    if (ext == "map") return FileKind::TileMap;
    if (ext == "ptn") return FileKind::Pattern;
    if (ext == "avi") return FileKind::Video;
    if (ext == "bmp") return FileKind::Bitmap;
    if (ext == "fon") return FileKind::Font;
    if (ext == "txt" || ext == "doc") return FileKind::Text;
    return FileKind::Unknown;
}

std::vector<CatalogEntry> build_catalog(const DiscImage& disc) {
    std::vector<CatalogEntry> catalog;
    for (auto& file : list_files(disc)) {
        if (!is_in_data_track(disc, file)) continue;
        // Só o início de cada arquivo interessa ao classificador.
        auto head = read_file(disc, {file.path, file.lba, std::min<uint32_t>(file.size, kHeadBytes)});
        auto kind = classify(file.path, head);
        catalog.push_back({std::move(file), kind});
    }
    return catalog;
}

const char* kind_name(FileKind kind) {
    switch (kind) {
        case FileKind::MainExecutable: return "executavel";
        case FileKind::Overlay: return "overlay";
        case FileKind::Graphics: return "graficos";
        case FileKind::TileMap: return "mapa";
        case FileKind::Pattern: return "padrao";
        case FileKind::MusicStream: return "musica";
        case FileKind::SoundBank: return "banco-som";
        case FileKind::SoundRaw: return "som-cru";
        case FileKind::Video: return "video";
        case FileKind::Bitmap: return "bitmap";
        case FileKind::Font: return "fonte";
        case FileKind::Text: return "texto";
        case FileKind::Unknown: return "desconhecido";
    }
    return "desconhecido";
}

}  // namespace saturnfs
