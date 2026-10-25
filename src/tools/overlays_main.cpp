#include <iomanip>
#include <iostream>

#include "saturnfs/image.hpp"
#include "saturnfs/catalog.hpp"
#include "saturnfs/overlay.hpp"
#include "saturnfs/verify.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "uso: dxpc-overlays <arquivo.cue>\n";
        return 2;
    }
    try {
        auto disc = saturnfs::DiscImage::open(argv[1]);
        saturnfs::require_supported_disc(saturnfs::read_header(disc));
        saturnfs::require_complete_image(disc);

        std::vector<saturnfs::OverlayInfo> overlays;
        std::cout << std::hex << std::setfill('0');
        for (const auto& entry : saturnfs::build_catalog(disc)) {
            if (entry.kind != saturnfs::FileKind::Overlay) continue;
            if (!saturnfs::known_overlay_base(entry.file.path)) {
                std::cout << entry.file.path << "  (dados, sem endereço de carga)\n";
                continue;
            }
            auto info = saturnfs::parse_overlay(entry.file.path, saturnfs::read_file(disc, entry.file));
            std::cout << std::setw(14) << std::setfill(' ') << std::left << info.name << std::right << std::setfill('0')
                      << " 0x" << std::setw(8) << info.load_address << "-0x" << std::setw(8) << info.load_address + info.size
                      << std::dec << "  tabela=" << info.header_table.size() << std::hex << '\n';
            overlays.push_back(std::move(info));
        }
        std::cout << "--- regiões compartilhadas com base diferente (confirmar exclusividade)\n";
        for (const auto& c : saturnfs::find_overlay_conflicts(overlays)) std::cout << c << '\n';
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        return 1;
    }
}
