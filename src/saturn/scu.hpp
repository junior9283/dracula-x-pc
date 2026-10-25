#pragma once
#include <cstdint>
#include <functional>

#include "saturn/bus.hpp"

namespace saturn {

// System Control Unit (registradores em 0x25FE0000): máscara/estado de interrupções, DMA (níveis 0–2, modo direto) e timers.
// Entrega as interrupções à CPU mestre por `deliver(nível, vetor)`; a CPU avisa o atendimento por `acknowledge(vetor)`.
class Scu : public IoDevice {
public:
    // O valor é o índice do bit em IMS/IST.
    enum class Interrupt {
        VBlankIn, VBlankOut, HBlankIn, Timer0, Timer1, DspEnd, SoundRequest, SystemManager, Pad,
        Dma2End, Dma1End, Dma0End, DmaIllegal, SpriteEnd
    };

    explicit Scu(Bus& bus) : bus_(bus) {}

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;

    void raise(Interrupt source);
    void acknowledge(int vector);

    std::function<void(int level, int vector)> deliver;

private:
    uint32_t read_register(uint32_t offset) const;
    void write_register(uint32_t offset, uint32_t value);
    void update_pending();
    void run_dma(int level);

    Bus& bus_;
    uint32_t regs_[0x100 / 4] = {};
    uint32_t ims_ = 0xFFFFFFFF, ist_ = 0;
    uint32_t line_counter_ = 0;
};

}  // namespace saturn
