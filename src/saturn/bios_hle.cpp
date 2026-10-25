#include "saturn/bios_hle.hpp"

#include <cstdio>

namespace saturn {
namespace {

constexpr uint32_t kSlotSetVector = 0x06000300;       // r4 = vetor, r5 = handler: grava na tabela de vetores
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
    register_service(kSlotScuMask, [this](sh2::Cpu& cpu) {
        auto& s = cpu.state();
        const uint32_t old = scu_.read(0xA0, 4);
        scu_.write(0xA0, 4, (old & s.r[4]) | s.r[5]);
        s.r[0] = old;
    });
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

    // Vetores de interrupção: handler padrão do BIOS (rte); vetores de exceção: um gatilho fatal por vetor.
    for (uint32_t vector = 0; vector < kVectorCount; ++vector)
        bus_.write32(kVectorBase + vector * 4, vector >= kFirstInterruptVector ? trap_address(kInterruptIndexBase + vector - kFirstInterruptVector) : trap_address(kExceptionIndexBase + vector));
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
    if (index >= kExceptionIndexBase)
        throw MachineError("exceção sem tratamento: vetor " + std::to_string(index - kExceptionIndexBase) + ", PC salvo " + hex(bus_.read32(s.r[15])));

    const uint32_t slot = index < kRamSlotIndex ? index * 4 : kServiceTableBase + (index - kRamSlotIndex) * 4;
    s.pc = s.pr;  // retorno por rts; o serviço pode sobrescrever o PC
    if (auto service = services_.find(slot); service != services_.end()) {
        service->second(cpu);
    } else {
        if (strict_)
            throw MachineError("serviço do BIOS não implementado: slot " + hex(slot) + " (retorno " + hex(s.pr) + ", r4=" + hex(s.r[4]) + ", r5=" + hex(s.r[5]) + ")");
        if (calls_.size() < kMaxLoggedCalls) calls_.push_back({slot, s.pr, {s.r[4], s.r[5], s.r[6], s.r[7]}});
    }
    return true;
}

}  // namespace saturn
