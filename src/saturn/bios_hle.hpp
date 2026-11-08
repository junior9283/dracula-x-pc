#pragma once
#include <cstdint>
#include <functional>
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
    // Ponto de entrada do escravo = vetor 0x94 da tabela do VBR (gravado por SYS_SETSINT).
    static constexpr uint32_t kSlaveEntryVector = 0x94, kSlaveEntryPointer = kVectorBase + kSlaveEntryVector * 4;
    static constexpr uint32_t kServiceTableBase = 0x06000200, kServiceTableEnd = 0x06000400;
    static constexpr uint32_t kHandlerTable = 0x06000900;                       // tratadores do SCU registrados pelo jogo: + 4·vetor (0x40 → 0x06000A00), como no BIOS
    static constexpr uint32_t kRomTableSize = 0x1000;                           // ponteiros de serviço na ROM (0x00000000–0x00000FFF)
    static constexpr uint32_t kTrapBase = 0x00008000, kTrapEnd = 0x0000A000;    // 4 bytes por gatilho, na área da ROM da BIOS
    static constexpr uint32_t kBackupFunctionCount = 12;
    // Os gatilhos BUP ocupam uma faixa reservada, separada dos ponteiros ROM/RAM e dos vetores.
    static constexpr uint32_t kBackupIndexBase = 0x540;
    static constexpr uint32_t backup_trap_address(uint32_t function_index) {
        return kTrapBase + 4 * (kBackupIndexBase + function_index);
    }

    // Chamada a um serviço do BIOS sem implementação (registrada no modo tolerante).
    struct Call {
        uint32_t slot;              // endereço do ponteiro de serviço (ROM ou RAM)
        uint32_t return_address;    // PR no momento da chamada
        std::array<uint32_t, 4> args;  // r4–r7
    };

    // Serviço nativo: chamado com PC já igual ao PR (retorno por rts); pode alterar o PC para não retornar (ex.: carga e salto).
    using Service = std::function<void(sh2::Cpu&)>;

    BiosHle(Bus& bus, Scu& scu);

    // Associa um serviço ao endereço do slot (ROM, 0x00000000–0x00000FFF, ou RAM, 0x06000200–0x060003FF).
    void register_service(uint32_t slot, Service service) { services_[slot] = std::move(service); }
    void register_backup_service(uint32_t function_index, Service service);

    // Preenche as tabelas e ajusta VBR da CPU.
    void install(sh2::Cpu& cpu);
    // Se o PC da CPU é um gatilho, executa o serviço e devolve true. Lança MachineError em serviço/exceção sem tratamento.
    bool handle(sh2::Cpu& cpu);

    static bool is_trap(uint32_t pc) { return pc >= kTrapBase && pc < kTrapEnd; }

    // Estrito (padrão): serviço sem implementação lança MachineError. Tolerante: registra a chamada e volta ao chamador.
    void set_strict(bool strict) { strict_ = strict; }
    const std::vector<Call>& calls() const { return calls_; }

private:
    void return_from_interrupt(sh2::Sh2State& state);
    void log_call(uint32_t slot, const sh2::Sh2State& state);  // modo tolerante: registra a chamada sem implementação
    static uint32_t default_vector(uint32_t vector);  // gatilho padrão do BIOS para o vetor
    // Índice do gatilho: [0, 0x400) ROM; [0x400, 0x480) RAM; [0x500, 0x540) interrupções;
    // [0x540, 0x54C) backup; 0x580 retorno da interrupção; 0x600+v exceção.
    static constexpr uint32_t kRamSlotIndex = 0x400, kInterruptIndexBase = 0x500, kInterruptReturnIndex = 0x580, kExceptionIndexBase = 0x600;
    static constexpr uint32_t kFrameWords = 11;  // r0–r7, PR, MACH, MACL salvos pelo trampolim
    static uint32_t trap_address(uint32_t index) { return kTrapBase + 4 * index; }

    Bus& bus_;
    Scu& scu_;
    bool strict_ = true;
    std::map<uint32_t, Service> services_;
    std::map<uint32_t, Service> backup_services_;
    std::vector<Call> calls_;
    std::vector<uint8_t> rom_table_;
};

}  // namespace saturn
