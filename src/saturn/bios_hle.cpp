#include "saturn/bios_hle.hpp"

#include <cstdio>

namespace saturn {
namespace {

constexpr uint32_t kSlotSetVector = 0x06000300;       // SYS_SETUINT: r4 = vetor, r5 = tratador do SCU (chamado pelo trampolim)
constexpr uint32_t kSlotGetVector = 0x06000304;       // SYS_GETUINT: r0 = tratador do SCU do vetor r4
constexpr uint32_t kSlotSetSh2Vector = 0x06000310;    // SYS_SETSINT: grava r5 direto em VBR + 4·r4 (r5 = 0 restaura o padrão)
constexpr uint32_t kSlotGetSh2Vector = 0x06000314;    // SYS_GETSINT: r0 = [VBR + 4·r4]
constexpr uint32_t kSlotScuMask = 0x06000344;         // (IMS & r4) | r5 ; devolve o IMS antigo em r0
constexpr uint32_t kVectorCount = 0x80;               // vetores 0–0x7F cabem antes da tabela de serviços (0x06000200)
constexpr uint32_t kFirstInterruptVector = 0x40;
constexpr uint32_t kSystemVariablesBegin = 0x06000240, kSystemVariablesEnd = 0x06000260;  // dados, não ponteiros: começam em 0
constexpr size_t kMaxLoggedCalls = 100000;


std::string hex(uint32_t v) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08X", v);
    return buffer;
}

}  // namespace

BiosHle::BiosHle(Bus& bus, Scu& scu) : bus_(bus), scu_(scu) {
    register_service(kSlotSetVector, [this](sh2::Cpu& cpu) {
        const auto& s = cpu.state();
        bus_.write32(kHandlerTable + s.r[4] * 4, s.r[5]);
    });
    register_service(kSlotGetVector, [this](sh2::Cpu& cpu) {
        auto& s = cpu.state();
        s.r[0] = bus_.read32(kHandlerTable + s.r[4] * 4);
    });
    // A tabela é a do VBR de quem chama; o vetor 0x94 cai em 0x06000250, o ponto de entrada do escravo.
    register_service(kSlotSetSh2Vector, [this](sh2::Cpu& cpu) {
        const auto& s = cpu.state();
        bus_.write32(s.vbr + s.r[4] * 4, s.r[5] != 0 ? s.r[5] : default_vector(s.r[4]));
    });
    register_service(kSlotGetSh2Vector, [this](sh2::Cpu& cpu) {
        auto& s = cpu.state();
        s.r[0] = bus_.read32(s.vbr + s.r[4] * 4);
    });
    register_service(kSlotScuMask, [this](sh2::Cpu& cpu) {
        auto& s = cpu.state();
        const uint32_t old = scu_.read(0xA0, 4);
        scu_.write(0xA0, 4, (old & s.r[4]) | s.r[5]);
        s.r[0] = old;
    });
}

void BiosHle::register_backup_service(uint32_t function_index, Service service) {
    if (function_index >= kBackupFunctionCount) throw std::out_of_range("índice de serviço BUP inválido");
    backup_services_[function_index] = std::move(service);
}

uint32_t BiosHle::default_vector(uint32_t vector) {
    if (vector >= kVectorCount) return 0;  // além da tabela: variáveis de sistema (ex.: 0x94 → 0x06000250)
    return vector >= kFirstInterruptVector ? trap_address(kInterruptIndexBase + vector - kFirstInterruptVector) : trap_address(kExceptionIndexBase + vector);
}

void BiosHle::log_call(uint32_t slot, const sh2::Sh2State& s) {
    if (calls_.size() < kMaxLoggedCalls) calls_.push_back({slot, s.pr, {s.r[4], s.r[5], s.r[6], s.r[7]}});
}

void BiosHle::return_from_interrupt(sh2::Sh2State& s) {  // rte: PC e SR empilhados pela entrada da exceção
    s.pc = bus_.read32(s.r[15]);
    s.r[15] += 4;
    s.sr = bus_.read32(s.r[15]) & sh2::kSrWritable;
    s.r[15] += 4;
}

void BiosHle::install(sh2::Cpu& cpu) {
    // Tabela de ponteiros da ROM (0x00000000+): cada entrada aponta para o seu próprio gatilho.
    rom_table_.assign(kRomTableSize, 0);
    for (uint32_t i = 0; i < kRomTableSize / 4; ++i)
        for (int b = 0; b < 4; ++b) rom_table_[i * 4 + b] = (trap_address(i) >> (8 * (3 - b))) & 0xFF;
    bus_.map_rom(0, rom_table_);

    // Slots de serviço na RAM: um gatilho por slot; as variáveis de sistema ficam zeradas.
    for (uint32_t slot = kServiceTableBase; slot < kServiceTableEnd; slot += 4)
        bus_.write32(slot, (slot >= kSystemVariablesBegin && slot < kSystemVariablesEnd) ? 0 : trap_address(kRamSlotIndex + (slot - kServiceTableBase) / 4));
    // Este slot é um ponteiro para a tabela da biblioteca de backup (BUP_Init
    // em 0x06000358 irá preenchê-lo), e não um endereço de serviço executável.
    bus_.write32(0x06000354, 0);

    // Vetores de interrupção: handler padrão do BIOS (rte); vetores de exceção: um gatilho fatal por vetor.
    for (uint32_t vector = 0; vector < kVectorCount; ++vector) bus_.write32(kVectorBase + vector * 4, default_vector(vector));
    cpu.state().vbr = kVectorBase;
}

bool BiosHle::handle(sh2::Cpu& cpu) {
    auto& s = cpu.state();
    if (!is_trap(s.pc)) return false;
    const uint32_t index = (s.pc - kTrapBase) / 4;

    if (index >= kInterruptIndexBase && index < kInterruptIndexBase + (kVectorCount - kFirstInterruptVector)) {
        // Trampolim do BIOS: chama o tratador registrado pelo jogo como sub-rotina; sem tratador, só retorna da interrupção.
        const uint32_t handler = bus_.read32(kHandlerTable + (kFirstInterruptVector + index - kInterruptIndexBase) * 4);
        if (handler == 0) { return_from_interrupt(s); return true; }
        s.r[15] -= 4 * kFrameWords;
        for (uint32_t i = 0; i < 8; ++i) bus_.write32(s.r[15] + 4 * i, s.r[i]);
        bus_.write32(s.r[15] + 32, s.pr);
        bus_.write32(s.r[15] + 36, s.mach);
        bus_.write32(s.r[15] + 40, s.macl);
        s.pr = trap_address(kInterruptReturnIndex);
        s.pc = handler;
        return true;
    }
    if (index == kInterruptReturnIndex) {  // o tratador voltou: restaura o contexto salvo e executa o rte
        for (uint32_t i = 0; i < 8; ++i) s.r[i] = bus_.read32(s.r[15] + 4 * i);
        s.pr = bus_.read32(s.r[15] + 32);
        s.mach = bus_.read32(s.r[15] + 36);
        s.macl = bus_.read32(s.r[15] + 40);
        s.r[15] += 4 * kFrameWords;
        return_from_interrupt(s);
        return true;
    }
    if (index >= kBackupIndexBase && index < kBackupIndexBase + kBackupFunctionCount) {
        const uint32_t function_index = index - kBackupIndexBase;
        s.pc = s.pr;
        if (const auto service = backup_services_.find(function_index); service != backup_services_.end()) {
            service->second(cpu);
        } else {
            if (strict_)
                throw MachineError("serviço BUP não implementado: índice " + std::to_string(function_index) + " (retorno " + hex(s.pr) + ")");
            // Marcador de diagnóstico fora do espaço de slots reais da BIOS.
            log_call(0xFFFF0000u + function_index * 4, s);
        }
        return true;
    }
    if (index >= kExceptionIndexBase)
        throw MachineError("exceção sem tratamento: vetor " + std::to_string(index - kExceptionIndexBase) + ", PC salvo " + hex(bus_.read32(s.r[15])));

    const uint32_t slot = index < kRamSlotIndex ? index * 4 : kServiceTableBase + (index - kRamSlotIndex) * 4;
    s.pc = s.pr;  // retorno por rts; o serviço pode sobrescrever o PC
    if (auto service = services_.find(slot); service != services_.end()) {
        service->second(cpu);
    } else {
        if (strict_)
            throw MachineError("serviço do BIOS não implementado: slot " + hex(slot) + " (retorno " + hex(s.pr) + ", r4=" + hex(s.r[4]) + ", r5=" + hex(s.r[5]) + ")");
        log_call(slot, s);
    }
    return true;
}

}  // namespace saturn
