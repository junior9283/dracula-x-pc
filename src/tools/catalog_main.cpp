#include <iostream>
#include <map>

#include "saturnfs/catalog.hpp"
#include "saturnfs/verify.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "uso: dxpc-catalog <arquivo.cue>\n";
        return 2;
    }
    try {
        auto disc = saturnfs::DiscImage::open(argv[1]);
        saturnfs::require_supported_disc(saturnfs::read_header(disc));
        saturnfs::require_complete_image(disc);

        std::map<std::string, std::pair<size_t, uint64_t>> totals;
        for (const auto& entry : saturnfs::build_catalog(disc)) {
            const char* kind = saturnfs::kind_name(entry.kind);
            std::cout << kind << ' ' << entry.file.size << ' ' << entry.file.path << '\n';
            totals[kind].first++;
            totals[kind].second += entry.file.size;
        }
        std::cout << "--- totais\n";
        for (const auto& [kind, t] : totals) std::cout << kind << ": " << t.first << " arquivos, " << t.second << " bytes\n";
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        return 1;
    }
}
