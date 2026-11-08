// Renderiza um dump de VDP1/VDP2 gravado por `dxpc-run --dump-video prefixo` (uso local, para iterar no renderizador sem rodar o jogo).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "video/vdp1.hpp"
#include "video/vdp2.hpp"

namespace {
std::vector<uint8_t> load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("não foi possível abrir " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "uso: dxpc-render-dump <prefixo> <saida.png> [máscara de camadas: bits 0-3 NBG0..3, bit 4 sprites]\n";
        return 2;
    }
    try {
        const std::string prefix = argv[1];
        const auto vram = load(prefix + ".vdp2-vram"), cram = load(prefix + ".vdp2-cram"), regs = load(prefix + ".vdp2-regs");
        const auto vdp1 = load(prefix + ".vdp1-vram");
        const unsigned mask = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 0)) : 0x3F;
        if (const char* bench = std::getenv("DXPC_BENCH")) {   // medição: renderiza N vezes e imprime ms/quadro
            const int n = std::max(1, std::atoi(bench));
            using clock = std::chrono::steady_clock;
            double ms1 = 0, ms2 = 0;
            for (int i = 0; i < n; ++i) {
                const auto t0 = clock::now();
                const auto sp = video::render_vdp1(vdp1);
                const auto t1 = clock::now();
                const auto fr = video::render_vdp2({vram, cram, regs}, mask, &sp);
                const auto t2 = clock::now();
                ms1 += std::chrono::duration<double, std::milli>(t1 - t0).count();
                ms2 += std::chrono::duration<double, std::milli>(t2 - t1).count();
                if (fr.pixels.empty()) return 1;
            }
            std::cout << "vdp1: " << ms1 / n << " ms/quadro, vdp2: " << ms2 / n << " ms/quadro, total: " << (ms1 + ms2) / n << " ms/quadro\n";
        }
        const auto sprites = video::render_vdp1(vdp1);
        if (std::getenv("DXPC_HIST")) {
            std::map<uint16_t, int> hist;
            for (uint16_t v : sprites.data) ++hist[v];
            std::vector<std::pair<int, uint16_t>> top;
            for (auto& [v, n] : hist) top.emplace_back(n, v);
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 6; ++i) std::cout << "valor 0x" << std::hex << top[i].second << std::dec << ": " << top[i].first << '\n';
        }
        video::write_png(argv[2], video::render_vdp2({vram, cram, regs}, mask, &sprites));
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
