#pragma once
#include <cstdint>

#include "saturn/bus.hpp"
#include "saturn/scu.hpp"

namespace saturn {

// Temporização de vídeo NTSC (262 linhas, 60 Hz): gera VBlank-IN/OUT e HBlank-IN no SCU e expõe o TVSTAT. Mapeável em 0x25F80000
// (por enquanto só o registrador TVSTAT em +0x04; os registradores do VDP2 entram na etapa 3).
class VideoTiming : public IoDevice {
public:
    static constexpr uint32_t kLines = 262;
    static constexpr uint32_t kVBlankLine = 224;
    static constexpr uint32_t kCyclesPerLine = 1820;                   // 28,636 MHz / (262 × ~60 Hz)
    static constexpr uint32_t kHBlankStart = 1500;                     // ciclo da linha em que o HBlank começa
    static constexpr uint64_t kCyclesPerFrame = uint64_t(kLines) * kCyclesPerLine;

    explicit VideoTiming(Scu& scu) : scu_(scu) {}

    void advance(uint64_t cycles);
    uint16_t tvstat() const;
    uint64_t frame() const { return frame_; }

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t, int, uint32_t) override {}

private:
    Scu& scu_;
    uint32_t line_ = 0, cycle_ = 0;   // posição dentro do quadro
    bool hblank_ = false, vblank_ = false;
    uint64_t frame_ = 0;
};

}  // namespace saturn
