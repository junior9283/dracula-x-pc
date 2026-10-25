#pragma once
#include <cstdint>
#include <map>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "saturn/bus.hpp"
#include "saturn/scu.hpp"
#include "sh2/cpu.hpp"

namespace saturn {

// Erro de emulação com mensagem para o desenvolvedor (serviço do BIOS não implementado, exceção sem tratamento…).
struct MachineError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// BIOS por software (HLE): sem ROM real. Preenche a tabela de ponteiros de serviço (0x06000200–0x060003FF) e a tabela de vetores
// (VBR = 0x06000000) com endereços de "gatilho" na área da ROM; quando a CPU chega a um gatilho, o serviço nativo roda e emula o
// retorno (rts, ou rte em vetores). Serviços são implementados por demanda: um slot sem serviço para a execução com erro claro.
class BiosHle {
public:
    static constexpr uint32_t kVectorBase = 0x06000000;
    static constexpr uint32_t kServiceTableBase = 0x06000200, kServiceTableEnd = 0x06000400;
    static constexpr uint32_t kRomTableSize = 0x1000;                           // ponteiros de serviço na ROM (0x00000000–0x00000FFF)
    static constexpr uint32_t kTrapBase = 0x00008000, kTrapEnd = 0x0000A000;    // 4 bytes por gatilho, na área da ROM da BIOS

    // Chamada a um serviço do BIOS sem implementação (registrada no modo tolerante).
    struct Call {
        uint32_t slot;              // endereço do ponteiro de serviço (ROM ou RAM)
        uint32_t return_address;    // PR no momento da chamada
        std::array<uint32_t, 4> args;  // r4–r7
    };

    BiosHle(Bus& bus, Scu& scu);

    // Preenche as tabelas e ajusta VBR da CPU.
    void install(sh2::Cpu& cpu);
    // Se o PC da CPU é um gatilho, executa o serviço e devolve true. Lança MachineError em serviço/exceção sem tratamento.
    bool handle(sh2::Cpu& cpu);

    static bool is_trap(uint32_t pc) { return pc >= kTrapBase && pc < kTrapEnd; }

    // Estrito (padrão): serviço sem implementação lança MachineError. Tolerante: registra a chamada e volta ao chamador.
    void set_strict(bool strict) { strict_ = strict; }
    const std::vector<Call>& calls() const { return calls_; }

private:
    // Índice do gatilho: [0, 0x400) ponteiros da ROM (índice = offset/4); [0x400, 0x480) slots de RAM; 0x500 = rte; 0x600+v = vetor v.
    static constexpr uint32_t kRamSlotIndex = 0x400, kDefaultVectorIndex = 0x500, kExceptionIndexBase = 0x600;
    static uint32_t trap_address(uint32_t index) { return kTrapBase + 4 * index; }

    Bus& bus_;
    Scu& scu_;
    bool strict_ = true;
    std::vector<Call> calls_;
    std::vector<uint8_t> rom_table_;
};

}  // namespace saturn
