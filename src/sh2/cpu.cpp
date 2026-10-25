#include "sh2/cpu.hpp"

#include <stdexcept>
#include <string_view>

namespace sh2 {
namespace {
constexpr int kVectorIllegal = 4;
constexpr int kVectorAddressError = 9;
constexpr uint32_t kExceptionCycles = 8;
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
    if (a & 1) throw AddressError{a};
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(bus_.read16(a))));
}
uint32_t Cpu::load32(uint32_t a) {
    if (a & 3) throw AddressError{a};
    return bus_.read32(a);
}
void Cpu::store8(uint32_t a, uint32_t v) { bus_.write8(a, static_cast<uint8_t>(v)); }
void Cpu::store16(uint32_t a, uint32_t v) {
    if (a & 1) throw AddressError{a};
    bus_.write16(a, static_cast<uint16_t>(v));
}
void Cpu::store32(uint32_t a, uint32_t v) {
    if (a & 3) throw AddressError{a};
    bus_.write32(a, v);
}

void Cpu::enter_exception(int vector, uint32_t saved_pc) {
    s_.r[15] -= 4;
    bus_.write32(s_.r[15], s_.sr);
    s_.r[15] -= 4;
    bus_.write32(s_.r[15], saved_pc);
    s_.pc = bus_.read32(s_.vbr + static_cast<uint32_t>(vector) * 4);
}

uint32_t Cpu::step() {
    op_address_ = s_.pc;
    try {
        if (s_.pc & 1) throw AddressError{s_.pc};
        const uint16_t op = bus_.read16(s_.pc);
        s_.pc += 2;
        if (OpHandler handler = table()[op]) return handler(*this, op);
        enter_exception(kVectorIllegal, op_address_);
    } catch (const AddressError&) {
        enter_exception(kVectorAddressError, op_address_);
    }
    return kExceptionCycles;
}

}  // namespace sh2
