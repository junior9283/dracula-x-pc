#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace video {

// Quadro de sprites do VDP1: 512 x 256 palavras de 16 bits (0 = sem sprite), no formato que o VDP1 grava no frame buffer.
struct SpriteFrame {
    static constexpr int kWidth = 512, kHeight = 256;
    std::vector<uint16_t> data = std::vector<uint16_t>(size_t(kWidth) * kHeight, 0);
};

// Executa a lista de comandos do VDP1 guardada na VRAM (32 bytes por comando, a partir do endereço 0) e devolve o resultado.
// Implementa sprites normais/escalados/distorcidos, polígonos, linhas, polilinhas e os comandos de recorte/coordenada local.
SpriteFrame render_vdp1(std::span<const uint8_t> vram);

}  // namespace video
