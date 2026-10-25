#include "saturn/bios_hle.hpp"

#include <cstdio>

namespace saturn {
namespace {

constexpr uint32_t kSlotSetVector = 0x06000300;       // r4 = vetor, r5 = handler: grava na tabela de vetores
constexpr uint32_t kSlotScuMask = 0x06000344;         // (IMS & r4) | r5 ; devolve o IMS antigo em r0
constexpr uint32_t kVectorCount = 0x80;               // vetores 0–0x7F cabem antes da tabela de serviços (0x06000200)
constexpr uint32_t kFirstInterruptVector = 0x40;
constexpr uint32_t kSystemVariablesBegin = 0x06000240, kSystemVariablesEnd = 0x06000260;  // dados, não ponteiros: começam em 0
constexpr uint32_t kDefaultVectorIndex = 0x100, kExceptionIndexBase = 0x200;

std::string hex(uint32_t v) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08X", v);
    return buffer;
}

}  // namespace

BiosHle::BiosHle(Bus& bus, Scu& scu) : bus_(bus), scu_(scu) {}

void BiosHle::install(sh2::Cpu& cpu) {
    // Cada slot de serviço aponta para o seu próprio gatilho; as variáveis de sistema ficam zeradas.
    for (uint32_t slot = kServiceTableBase; slot < kServiceTableEnd; slot += 4)
        bus_.write32(slot, (slot >= kSystemVariablesBegin && slot < kSystemVariablesEnd) ? 0 : trap_for_slot(slot));

    // Vetores de interrupção: handler padrão do BIOS (rte); vetores de exceção: um gatilho fatal por vetor.
    for (uint32_t vector = 0; vector < kVectorCount; ++vector) {
        const uint32_t trap = vector >= kFirstInterruptVector ? kTrapBase + 4 * kDefaultVectorIndex : kTrapBase + 4 * (kExceptionIndexBase + vector);
        bus_.write32(kVectorBase + vector * 4, trap);
    }
    cpu.state().vbr = kVectorBase;
}

bool BiosHle::handle(sh2::Cpu& cpu) {
    auto& s = cpu.state();
    if (!is_trap(s.pc)) return false;
    const uint32_t index = (s.pc - kTrapBase) / 4;

    if (index == kDefaultVectorIndex) {  // rte: o jogo não instalou handler para esse vetor
        s.pc = bus_.read32(s.r[15]);
        s.r[15] += 4;
        s.sr = bus_.read32(s.r[15]) & sh2::kSrWritable;
        s.r[15] += 4;
        return true;
    }
    if (index >= kExceptionIndexBase) {
        throw MachineError("exceção sem tratamento: vetor " + std::to_string(index - kExceptionIndexBase) + ", PC salvo " + hex(bus_.read32(s.r[15])));
    }

    const uint32_t slot = kServiceTableBase + index * 4;
    switch (slot) {
        case kSlotSetVector:
            bus_.write32(kVectorBase + s.r[4] * 4, s.r[5]);
            break;
        case kSlotScuMask: {
            const uint32_t old = scu_.read(0xA0, 4);
            scu_.write(0xA0, 4, (old & s.r[4]) | s.r[5]);
            s.r[0] = old;
            break;
        }
        default:
            throw MachineError("serviço do BIOS não implementado: slot " + hex(slot) + " (retorno " + hex(s.pr) + ", r4=" + hex(s.r[4]) + ", r5=" + hex(s.r[5]) + ")");
    }
    s.pc = s.pr;  // rts
    return true;
}

}  // namespace saturn
