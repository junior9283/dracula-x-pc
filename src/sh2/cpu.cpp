#include "sh2/cpu.hpp"

#include <stdexcept>
#include <string_view>

namespace sh2 {
namespace {
constexpr int kVectorIllegal = 4;
constexpr int kVectorSlotIllegal = 6;
constexpr int kVectorAddressError = 9;
constexpr int kNmiLevel = 16;
constexpr uint32_t kExceptionCycles = 8;

// Instruções que não podem estar num delay slot: desvios e TRAPA.
bool is_slot_illegal(uint16_t op) {
    const uint16_t hi = op & 0xF000, mid = op & 0xFF00, reg = op & 0xF0FF;
    return hi == 0xA000 || hi == 0xB000                                                       // bra, bsr
           || mid == 0x8900 || mid == 0x8B00 || mid == 0x8D00 || mid == 0x8F00                  // bt, bf, bt/s, bf/s
           || reg == 0x402B || reg == 0x400B || reg == 0x0023 || reg == 0x0003                  // jmp, jsr, braf, bsrf
           || op == 0x000B || op == 0x002B || mid == 0xC300;                                    // rts, rte, trapa
}
}  // namespace

OpcodeTable::OpcodeTable() { handlers_.fill(nullptr); }

void OpcodeTable::add(const char* pattern, OpHandler handler) {
    const std::string_view text(pattern);
    if (text.size() != 16) throw std::logic_error("padrão de opcode deve ter 16 caracteres");
    uint16_t mask = 0, value = 0;
    for (char c : text) {
        mask <<= 1;
        value <<= 1;
        if (c == '0' || c == '1') { mask |= 1; value |= (c == '1'); }
    }
    for (uint32_t op = 0; op < handlers_.size(); ++op) {
        if ((op & mask) != value) continue;
        if (handlers_[op]) throw std::logic_error(std::string("opcode registrado duas vezes pelo padrão ") + pattern);
        handlers_[op] = handler;
    }
}

const OpcodeTable& Cpu::table() {
    static const OpcodeTable instance = [] {
        OpcodeTable t;
        OpRegistrar::data(t);
        OpRegistrar::alu(t);
        OpRegistrar::branch(t);
        return t;
    }();
    return instance;
}

Cpu::Cpu(saturn::Bus& bus, int id) : bus_(bus), id_(id) {}

void Cpu::reset(uint32_t pc, uint32_t sp) {
    s_ = {};
    s_.pc = pc;
    s_.r[15] = sp;
    s_.sr = kSrIMask;  // interrupções mascaradas (I = 15) após o reset
}

uint32_t Cpu::load8s(uint32_t a) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(bus_.read8(a)))); }
uint32_t Cpu::load16s(uint32_t a) {
    if (a & 1) throw Exception{kVectorAddressError, op_address_};
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(bus_.read16(a))));
}
uint32_t Cpu::load32(uint32_t a) {
    if (a & 3) throw Exception{kVectorAddressError, op_address_};
    return bus_.read32(a);
}
void Cpu::store8(uint32_t a, uint32_t v) { bus_.write8(a, static_cast<uint8_t>(v)); }
void Cpu::store16(uint32_t a, uint32_t v) {
    if (a & 1) throw Exception{kVectorAddressError, op_address_};
    bus_.write16(a, static_cast<uint16_t>(v));
}
void Cpu::store32(uint32_t a, uint32_t v) {
    if (a & 3) throw Exception{kVectorAddressError, op_address_};
    bus_.write32(a, v);
}

void Cpu::enter_exception(int vector, uint32_t saved_pc) {
    s_.r[15] -= 4;
    bus_.write32(s_.r[15], s_.sr);
    s_.r[15] -= 4;
    bus_.write32(s_.r[15], saved_pc);
    s_.pc = bus_.read32(s_.vbr + static_cast<uint32_t>(vector) * 4);
}

void Cpu::raise_interrupt(int level, int vector) {
    if (level < pending_level_) return;  // mantém a de maior prioridade
    pending_level_ = level;
    pending_vector_ = vector;
}

bool Cpu::take_interrupt_if_due() {
    if (pending_level_ == 0) return false;
    const int mask = (s_.sr & kSrIMask) >> 4;
    if (pending_level_ != kNmiLevel && pending_level_ <= mask) return false;

    const int level = pending_level_, vector = pending_vector_;
    pending_level_ = 0;
    sleeping_ = false;
    enter_exception(vector, s_.pc);
    s_.sr = (s_.sr & ~kSrIMask) | (static_cast<uint32_t>(level > 15 ? 15 : level) << 4);
    if (on_interrupt_taken) on_interrupt_taken(level, vector);
    return true;
}

uint32_t Cpu::delay_slot(uint32_t target) {
    const uint32_t slot_address = s_.pc;  // já aponta para a instrução depois do desvio
    const uint32_t branch_address = op_address_;
    if (slot_address & 1) throw Exception{kVectorAddressError, slot_address};
    const uint16_t slot = bus_.read16(slot_address);
    if (is_slot_illegal(slot)) throw Exception{kVectorSlotIllegal, branch_address};
    const OpHandler handler = table()[slot];
    if (!handler) throw Exception{kVectorIllegal, slot_address};

    s_.in_delay_slot = true;
    op_address_ = slot_address;
    s_.pc = slot_address + 2;
    const uint32_t cycles = handler(*this, slot);
    s_.in_delay_slot = false;
    op_address_ = branch_address;
    s_.pc = target;
    return cycles;
}

uint32_t Cpu::step() {
    const bool inhibited = inhibit_irq_;
    inhibit_irq_ = false;
    if (!inhibited && take_interrupt_if_due()) return kExceptionCycles;
    if (sleeping_) return 1;  // parado até a próxima interrupção

    op_address_ = s_.pc;
    try {
        if (s_.pc & 1) throw Exception{kVectorAddressError, s_.pc};
        const uint16_t op = bus_.read16(s_.pc);
        s_.pc += 2;
        if (OpHandler handler = table()[op]) return handler(*this, op);
        throw Exception{kVectorIllegal, op_address_};
    } catch (const Exception& e) {
        s_.in_delay_slot = false;
        enter_exception(e.vector, e.saved_pc);
    }
    return kExceptionCycles;
}

}  // namespace sh2
