#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "saturn/bus.hpp"
#include "saturn/state.hpp"

namespace saturn {

// DSP do SCU (registradores 0x80–0x8C do SCU): processador VLIW de 32 bits com 256 palavras de programa, 4 bancos de 64
// palavras de dados, ALU de 48 bits, multiplicador e DMA próprio pelo barramento D0. O BIOS o usa como motor de DMA na
// abertura (logo e som); sem ele o BIOS espera para sempre o fim do programa (PPAF.EX).
//
// Modelo: uma instrução a cada 2 ciclos do SH-2, com a busca adiantada de uma instrução (os saltos têm um slot de atraso,
// e LPS repete a instrução já buscada). O DMA do DSP termina na própria instrução (T0 nunca fica ligado).
class ScuDsp {
public:
    explicit ScuDsp(Bus& bus) : bus_(bus) {}

    // Registradores: 0x80 PPAF (controle/estado), 0x84 PPD (programa), 0x88 PDA (endereço de dados), 0x8C PDD (dados).
    uint32_t read(uint32_t offset);
    void write(uint32_t offset, uint32_t value);

    void advance(uint32_t cycles);
    void reset();   // reset do SCU: para o programa e zera registradores e flags (as RAMs de programa e de dados ficam)
    bool running() const { return executing_ && !paused_; }

    void save(StateWriter& w) const;
    void load(StateReader& r);

    std::function<void()> on_end_interrupt;   // ENDI: pedido de interrupção "DSP end" ao SCU

    // Acesso direto para os testes.
    std::array<uint32_t, 256>& program() { return program_; }
    std::array<std::array<uint32_t, 64>, 4>& data() { return data_; }

private:
    void step();
    void general(uint32_t instr, bool looped);
    void load_immediate(uint32_t instr, bool looped);
    void dma(uint32_t instr);
    void special(uint32_t instr);
    bool condition(uint32_t cond) const;
    void finish_program_dma();
    void load_pc(uint8_t pc);   // novo PC: descarta a instrução já buscada e o DMA de programa pendente

    Bus& bus_;
    std::array<uint32_t, 256> program_{};
    std::array<std::array<uint32_t, 64>, 4> data_{};
    std::array<uint8_t, 4> ct_{};
    uint64_t ac_ = 0, p_ = 0;   // 48 bits úteis
    uint32_t rx_ = 0, ry_ = 0, ra0_ = 0, wa0_ = 0;
    uint32_t next_instr_ = 0;
    uint16_t lop_ = 0;
    uint8_t pc_ = 0, top_ = 0, data_address_ = 0;
    bool next_looped_ = false;
    bool executing_ = false, paused_ = false;
    bool flag_z_ = false, flag_s_ = false, flag_c_ = false, flag_v_ = false, flag_end_ = false;
    int32_t credit_ = 0;
    std::array<uint32_t, 256> program_dma_{};   // DMA para a RAM de programa: aplicado no END (ou MVI PC/RA0/WA0)
    uint32_t program_dma_count_ = 0;
};

}  // namespace saturn
