#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

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
    static constexpr uint32_t kTrapBase = 0x00000400, kTrapEnd = 0x00001000;    // 4 bytes por gatilho

    BiosHle(Bus& bus, Scu& scu);

    // Preenche as tabelas e ajusta VBR da CPU.
    void install(sh2::Cpu& cpu);
    // Se o PC da CPU é um gatilho, executa o serviço e devolve true. Lança MachineError em serviço/exceção sem tratamento.
    bool handle(sh2::Cpu& cpu);

    static bool is_trap(uint32_t pc) { return pc >= kTrapBase && pc < kTrapEnd; }

private:
    uint32_t trap_for_slot(uint32_t slot) const { return kTrapBase + 4 * ((slot - kServiceTableBase) / 4); }
    uint32_t trap_for_default_vector() const { return kTrapBase + 4 * kDefaultVectorTrap; }
    uint32_t trap_for_exception() const { return kTrapBase + 4 * kExceptionTrap; }

    static constexpr uint32_t kDefaultVectorTrap = 0x100;   // índices após os slots de serviço
    static constexpr uint32_t kExceptionTrap = 0x101;

    Bus& bus_;
    Scu& scu_;
};

}  // namespace saturn
