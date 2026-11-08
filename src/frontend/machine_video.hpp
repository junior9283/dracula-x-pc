// Quadro de vídeo atual da máquina: lista de comandos do VDP1 renderizada + composição do VDP2.
#pragma once

#include <array>
#include <cstdint>

#include "saturn/machine.hpp"
#include "video/vdp1.hpp"
#include "video/vdp2.hpp"

namespace frontend {

// Memórias de vídeo vistas pelo SH-2 (endereços cache-through).
inline constexpr uint32_t kVdp1Vram = 0x25C00000, kVdp1VramSize = 0x80000;
inline constexpr uint32_t kVdp2Vram = 0x25E00000, kVdp2VramSize = 0x80000;
inline constexpr uint32_t kVdp2Cram = 0x25F00000, kVdp2CramSize = 0x1000;
inline constexpr uint32_t kVdp2Regs = 0x25F80000, kVdp2RegsSize = 0x120;

inline video::Frame capture_frame(saturn::Machine& machine, unsigned layer_mask = 0x3F) {
    std::array<uint8_t, kVdp2RegsSize> regs{};   // registradores não são RAM: lidos pelo barramento
    for (uint32_t i = 0; i < regs.size(); ++i) regs[i] = machine.bus().read8(kVdp2Regs + i);
    const video::SpriteFrame sprites = video::render_vdp1(machine.bus().ram_at(kVdp1Vram, kVdp1VramSize));
    return video::render_vdp2({machine.bus().ram_at(kVdp2Vram, kVdp2VramSize), machine.bus().ram_at(kVdp2Cram, kVdp2CramSize), regs},
                              layer_mask, &sprites);
}

}  // namespace frontend
