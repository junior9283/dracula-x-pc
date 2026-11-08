#pragma once
#include <cstdint>
#include <span>

#include "video/frame.hpp"
#include "video/vdp1.hpp"

namespace video {

// Estado do VDP2 visto pelo renderizador: VRAM (512 KiB), RAM de cores (4 KiB) e registradores (0x120 bytes), todos big-endian.
struct Vdp2State {
    std::span<const uint8_t> vram, cram, regs;
};

// Renderiza o quadro do VDP2 (fundo + NBG0–NBG3 + RBG0/RBG1). Devolve um quadro vazio se o display estiver desligado.
// `layer_mask` (bit i = NBGi, bit 4 = sprites, bit 5 = RBG0) permite isolar camadas em diagnósticos.
Frame render_vdp2(const Vdp2State& state, unsigned layer_mask = 0x3F, const SpriteFrame* sprites = nullptr);

}  // namespace video
