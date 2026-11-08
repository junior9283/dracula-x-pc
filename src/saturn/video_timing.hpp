#pragma once
#include <array>
#include <cstdint>

#include "saturn/bus.hpp"
#include "saturn/scu.hpp"
#include "saturn/state.hpp"

namespace saturn {

// Temporização de vídeo NTSC não entrelaçada: 263 linhas por quadro; a linha tem 427 pontos no modo de 320 (relógio da CPU de
// 26,85 MHz, 4 ciclos por ponto = 1708 ciclos) ou 455 no de 352 (28,64 MHz, 1820 ciclos), conforme o HRESO do TVMD. O VBlank começa
// ao fim das linhas visíveis (VRESO: 224 ou 240) e o HBlank ao fim dos pontos ativos. Gera VBlank-IN/OUT e HBlank-IN no SCU e expõe
// o TVSTAT. Mapeável em 0x25F80000 (TVSTAT em +0x04 reflete o tempo; os demais registradores do VDP2 são guardados aqui).
class VideoTiming : public IoDevice {
public:
    static constexpr uint32_t kLines = 263;
    // Por modo horizontal: pontos ativos e totais da linha (4 ciclos de CPU por ponto) e relógio da CPU.
    struct Mode { uint32_t active_dots, line_dots, master_hz; };
    static constexpr Mode kMode320{320, 427, 26'846'590}, kMode352{352, 455, 28'636'360};
    static constexpr uint32_t kCyclesPerDot = 4;
    // Teto de um quadro (modo de 352): limite para laços que esperam o próximo quadro.
    static constexpr uint64_t kMaxCyclesPerFrame = uint64_t(kLines) * kMode352.line_dots * kCyclesPerDot;

    uint32_t cycles_per_line() const { return mode().line_dots * kCyclesPerDot; }
    uint64_t cycles_per_frame() const { return uint64_t(kLines) * cycles_per_line(); }
    uint32_t hblank_start() const { return mode().active_dots * kCyclesPerDot; }
    uint32_t visible_lines() const { return (tvmd() >> 4) & 3 ? 240 : 224; }
    // Relógio da CPU (e do barramento) no modo atual; o SCSP tem relógio próprio e converte por ele.
    uint32_t master_hz() const { return mode().master_hz; }
    uint64_t frame_ns() const { return cycles_per_frame() * 1'000'000'000ull / master_hz(); }
    uint64_t cycles_to_frame_end() const {
        const uint64_t done = uint64_t(line_) * cycles_per_line() + cycle_;
        return done < cycles_per_frame() ? cycles_per_frame() - done : 1;
    }

    explicit VideoTiming(Scu& scu) : scu_(scu) {}

    void advance(uint64_t cycles);
    uint16_t tvstat() const;
    uint64_t frame() const { return frame_; }
    void save(StateWriter& w) const;
    void load(StateReader& r);

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;

private:
    static constexpr uint32_t kRegisterBytes = 0x120;
    uint16_t tvmd() const { return uint16_t(regs_[0] << 8 | regs_[1]); }
    const Mode& mode() const { return (regs_[1] & 1) ? kMode352 : kMode320; }   // HRESO bit 0 (byte baixo do TVMD): 352/704 pontos
    Scu& scu_;
    std::array<uint8_t, kRegisterBytes> regs_{};   // demais registradores do VDP2 (guardados)
    uint32_t line_ = 0, cycle_ = 0;   // posição dentro do quadro
    bool hblank_ = false, vblank_ = false;
    uint64_t frame_ = 0;
};

}  // namespace saturn
