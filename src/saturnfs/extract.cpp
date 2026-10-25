#include "saturnfs/extract.hpp"

#include <fstream>
#include <stdexcept>

#include "saturnfs/iso9660.hpp"
#include "saturnfs/sha256.hpp"

namespace fs = std::filesystem;

namespace saturnfs {
namespace {

bool is_inside(const fs::path& root, const fs::path& candidate) {
    auto rel = candidate.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

}  // namespace

size_t extract_all(const SectorReader& source, const fs::path& out, bool force) {
    if (fs::exists(out) && !fs::is_empty(out) && !force)
        throw std::runtime_error("a pasta de destino já tem conteúdo (use --force para sobrescrever): " + out.string());
    fs::create_directories(out);

    std::ofstream manifest(out / "manifest.txt");
    size_t extracted = 0;
    for (const auto& entry : list_files(source)) {
        if (!is_in_data_track(source, entry)) {
            manifest << "# ignorado (fora da faixa de dados): " << entry.path << '\n';
            continue;
        }
        auto target = out / entry.path;
        if (!is_inside(out, target)) throw std::runtime_error("caminho inseguro no disco: " + entry.path);

        auto bytes = read_file(source, entry);
        fs::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        if (!file) throw std::runtime_error("falha ao gravar " + target.string());
        manifest << entry.path << ' ' << entry.lba << ' ' << entry.size << ' ' << sha256_hex(bytes) << '\n';
        ++extracted;
    }
    if (!manifest) throw std::runtime_error("falha ao gravar o manifest.txt");
    return extracted;
}

}  // namespace saturnfs
