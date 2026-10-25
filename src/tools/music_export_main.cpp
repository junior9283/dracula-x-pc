#include <cmath>
#include <iostream>

#include "audio/dvi.hpp"
#include "audio/wav.hpp"
#include "saturnfs/image.hpp"
#include "saturnfs/catalog.hpp"
#include "saturnfs/verify.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "uso: dxpc-music-export <arquivo.cue> <pasta-saida>\n";
        return 2;
    }
    try {
        auto disc = saturnfs::DiscImage::open(argv[1]);
        saturnfs::require_supported_disc(saturnfs::read_header(disc));
        saturnfs::require_complete_image(disc);
        std::filesystem::create_directories(argv[2]);

        for (const auto& entry : saturnfs::build_catalog(disc)) {
            if (entry.kind != saturnfs::FileKind::MusicStream) continue;
            auto file = saturnfs::read_file(disc, entry.file);
            auto header = audio::parse_dvi_header(file);
            auto pcm = audio::decode_dvi(std::span<const uint8_t>(file).subspan(header.header_size), header);

            double energy = 0;
            for (int16_t s : pcm) energy += double(s) * s;
            auto stem = std::filesystem::path(entry.file.path).stem().string();
            audio::write_wav_pcm16(std::filesystem::path(argv[2]) / (stem + ".wav"), pcm, audio::kDviSampleRate, 2);
            std::cout << stem << ' ' << pcm.size() / 2 / double(audio::kDviSampleRate) << " s  laço=";
            if (header.loop_start_frames) std::cout << *header.loop_start_frames / double(audio::kDviSampleRate) << " s";
            else std::cout << "sem laço";
            std::cout << "  rms=" << (pcm.empty() ? 0.0 : std::sqrt(energy / pcm.size())) << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        return 1;
    }
}
