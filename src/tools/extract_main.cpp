#include <cstring>
#include <iostream>

#include "saturnfs/image.hpp"
#include "saturnfs/extract.hpp"
#include "saturnfs/verify.hpp"

int main(int argc, char** argv) {
    bool force = argc == 4 && std::strcmp(argv[3], "--force") == 0;
    if (argc != 3 && !force) {
        std::cerr << "uso: dxpc-extract <arquivo.cue> <pasta-saida> [--force]\n";
        return 2;
    }
    try {
        auto disc = saturnfs::DiscImage::open(argv[1]);
        auto header = saturnfs::read_header(disc);
        saturnfs::require_supported_disc(header);
        saturnfs::require_complete_image(disc);
        std::cout << "Disco: " << header.product_id << ' ' << header.version << " (" << header.release_date << ")\n";
        auto count = saturnfs::extract_all(disc, argv[2], force);
        std::cout << count << " arquivos extraídos para " << argv[2] << '\n';
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        return 1;
    }
}
