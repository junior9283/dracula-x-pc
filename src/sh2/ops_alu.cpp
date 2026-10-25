// Aritmética, comparação, lógica, deslocamento/rotação e controle do bit T.
#include "sh2/op_fields.hpp"

namespace sh2 {
namespace {

inline void set_t(Sh2State& s, bool value) { set_sr_flag(s, kSrT, value); }
inline bool get_t(const Sh2State& s) { return sr_flag(s, kSrT); }

}  // namespace

void OpRegistrar::alu(OpcodeTable& t) {
    // ---- Soma e subtração
    t.add("0011nnnnmmmm1100", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] += c.s_.r[m_of(op)]; return 1; });
    t.add("0111nnnniiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] += static_cast<uint32_t>(simm8(op)); return 1; });
    t.add("0011nnnnmmmm1110", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint64_t sum = uint64_t(c.s_.r[n_of(op)]) + c.s_.r[m_of(op)] + (get_t(c.s_) ? 1 : 0);
        c.s_.r[n_of(op)] = static_cast<uint32_t>(sum);
        set_t(c.s_, sum >> 32);
        return 1;
    });
    t.add("0011nnnnmmmm1111", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t a = c.s_.r[n_of(op)], b = c.s_.r[m_of(op)], result = a + b;
        set_t(c.s_, (~(a ^ b) & (a ^ result)) >> 31);
        c.s_.r[n_of(op)] = result;
        return 1;
    });
    t.add("0011nnnnmmmm1000", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] -= c.s_.r[m_of(op)]; return 1; });
    t.add("0011nnnnmmmm1010", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint64_t subtrahend = uint64_t(c.s_.r[m_of(op)]) + (get_t(c.s_) ? 1 : 0);
        const bool borrow = subtrahend > c.s_.r[n_of(op)];
        c.s_.r[n_of(op)] = static_cast<uint32_t>(c.s_.r[n_of(op)] - subtrahend);
        set_t(c.s_, borrow);
        return 1;
    });
    t.add("0011nnnnmmmm1011", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t a = c.s_.r[n_of(op)], b = c.s_.r[m_of(op)], result = a - b;
        set_t(c.s_, ((a ^ b) & (a ^ result)) >> 31);
        c.s_.r[n_of(op)] = result;
        return 1;
    });
    t.add("0110nnnnmmmm1011", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = 0u - c.s_.r[m_of(op)]; return 1; });
    t.add("0110nnnnmmmm1010", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint64_t subtrahend = uint64_t(c.s_.r[m_of(op)]) + (get_t(c.s_) ? 1 : 0);
        c.s_.r[n_of(op)] = static_cast<uint32_t>(0u - subtrahend);
        set_t(c.s_, subtrahend != 0);
        return 1;
    });
    t.add("0100nnnn00010000", [](Cpu& c, uint16_t op) -> uint32_t {  // dt
        set_t(c.s_, --c.s_.r[n_of(op)] == 0);
        return 1;
    });

    // ---- Comparações (resultado em T)
    t.add("0011nnnnmmmm0000", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, c.s_.r[n_of(op)] == c.s_.r[m_of(op)]); return 1; });
    t.add("0011nnnnmmmm0010", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, c.s_.r[n_of(op)] >= c.s_.r[m_of(op)]); return 1; });
    t.add("0011nnnnmmmm0011", [](Cpu& c, uint16_t op) -> uint32_t {
        set_t(c.s_, static_cast<int32_t>(c.s_.r[n_of(op)]) >= static_cast<int32_t>(c.s_.r[m_of(op)])); return 1;
    });
    t.add("0011nnnnmmmm0110", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, c.s_.r[n_of(op)] > c.s_.r[m_of(op)]); return 1; });
    t.add("0011nnnnmmmm0111", [](Cpu& c, uint16_t op) -> uint32_t {
        set_t(c.s_, static_cast<int32_t>(c.s_.r[n_of(op)]) > static_cast<int32_t>(c.s_.r[m_of(op)])); return 1;
    });
    t.add("0100nnnn00010001", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, static_cast<int32_t>(c.s_.r[n_of(op)]) >= 0); return 1; });
    t.add("0100nnnn00010101", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, static_cast<int32_t>(c.s_.r[n_of(op)]) > 0); return 1; });
    t.add("0010nnnnmmmm1100", [](Cpu& c, uint16_t op) -> uint32_t {  // cmp/str: algum byte correspondente igual
        const uint32_t x = c.s_.r[n_of(op)] ^ c.s_.r[m_of(op)];
        set_t(c.s_, !(x & 0xFF000000u) || !(x & 0x00FF0000u) || !(x & 0x0000FF00u) || !(x & 0x000000FFu));
        return 1;
    });
    t.add("10001000iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, c.s_.r[0] == static_cast<uint32_t>(simm8(op))); return 1; });

    // ---- Extensões
    t.add("0110nnnnmmmm1110", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = static_cast<uint32_t>(static_cast<int8_t>(c.s_.r[m_of(op)])); return 1; });
    t.add("0110nnnnmmmm1111", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = static_cast<uint32_t>(static_cast<int16_t>(c.s_.r[m_of(op)])); return 1; });
    t.add("0110nnnnmmmm1100", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.r[m_of(op)] & 0xFF; return 1; });
    t.add("0110nnnnmmmm1101", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.r[m_of(op)] & 0xFFFF; return 1; });

    // ---- Lógica entre registradores
    t.add("0010nnnnmmmm1001", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] &= c.s_.r[m_of(op)]; return 1; });
    t.add("0010nnnnmmmm1011", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] |= c.s_.r[m_of(op)]; return 1; });
    t.add("0010nnnnmmmm1010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] ^= c.s_.r[m_of(op)]; return 1; });
    t.add("0110nnnnmmmm0111", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = ~c.s_.r[m_of(op)]; return 1; });
    t.add("0010nnnnmmmm1000", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, (c.s_.r[n_of(op)] & c.s_.r[m_of(op)]) == 0); return 1; });

    // ---- Lógica com imediato em R0 (zero-extend) e em byte @(R0,GBR)
    t.add("11001001iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] &= d8_of(op); return 1; });
    t.add("11001011iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] |= d8_of(op); return 1; });
    t.add("11001010iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] ^= d8_of(op); return 1; });
    t.add("11001000iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { set_t(c.s_, (c.s_.r[0] & d8_of(op)) == 0); return 1; });
    t.add("11001101iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t a = c.s_.gbr + c.s_.r[0]; c.store8(a, c.load8u(a) & d8_of(op)); return 3;
    });
    t.add("11001111iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t a = c.s_.gbr + c.s_.r[0]; c.store8(a, c.load8u(a) | d8_of(op)); return 3;
    });
    t.add("11001110iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t a = c.s_.gbr + c.s_.r[0]; c.store8(a, c.load8u(a) ^ d8_of(op)); return 3;
    });
    t.add("11001100iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t {
        set_t(c.s_, (c.load8u(c.s_.gbr + c.s_.r[0]) & d8_of(op)) == 0); return 3;
    });
    t.add("0100nnnn00011011", [](Cpu& c, uint16_t op) -> uint32_t {  // tas.b @Rn
        const uint32_t a = c.s_.r[n_of(op)];
        const uint8_t value = c.load8u(a);
        set_t(c.s_, value == 0);
        c.store8(a, value | 0x80);
        return 4;
    });

    // ---- Deslocamentos e rotações de 1 bit
    const auto shift_left_1 = [](Cpu& c, uint16_t op) -> uint32_t { auto& r = c.s_.r[n_of(op)]; set_t(c.s_, r >> 31); r <<= 1; return 1; };
    t.add("0100nnnn00000000", shift_left_1);  // shll
    t.add("0100nnnn00100000", shift_left_1);  // shal (idêntico ao shll)
    t.add("0100nnnn00000001", [](Cpu& c, uint16_t op) -> uint32_t { auto& r = c.s_.r[n_of(op)]; set_t(c.s_, r & 1); r >>= 1; return 1; });
    t.add("0100nnnn00100001", [](Cpu& c, uint16_t op) -> uint32_t {
        auto& r = c.s_.r[n_of(op)]; set_t(c.s_, r & 1); r = static_cast<uint32_t>(static_cast<int32_t>(r) >> 1); return 1;
    });
    t.add("0100nnnn00000100", [](Cpu& c, uint16_t op) -> uint32_t { auto& r = c.s_.r[n_of(op)]; set_t(c.s_, r >> 31); r = (r << 1) | (r >> 31); return 1; });
    t.add("0100nnnn00000101", [](Cpu& c, uint16_t op) -> uint32_t { auto& r = c.s_.r[n_of(op)]; set_t(c.s_, r & 1); r = (r >> 1) | (r << 31); return 1; });
    t.add("0100nnnn00100100", [](Cpu& c, uint16_t op) -> uint32_t {
        auto& r = c.s_.r[n_of(op)]; const uint32_t carry_in = get_t(c.s_) ? 1 : 0; set_t(c.s_, r >> 31); r = (r << 1) | carry_in; return 1;
    });
    t.add("0100nnnn00100101", [](Cpu& c, uint16_t op) -> uint32_t {
        auto& r = c.s_.r[n_of(op)]; const uint32_t carry_in = get_t(c.s_) ? 1u << 31 : 0; set_t(c.s_, r & 1); r = (r >> 1) | carry_in; return 1;
    });

    // ---- Deslocamentos fixos (não alteram T)
    t.add("0100nnnn00001000", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] <<= 2; return 1; });
    t.add("0100nnnn00001001", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] >>= 2; return 1; });
    t.add("0100nnnn00011000", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] <<= 8; return 1; });
    t.add("0100nnnn00011001", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] >>= 8; return 1; });
    t.add("0100nnnn00101000", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] <<= 16; return 1; });
    t.add("0100nnnn00101001", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] >>= 16; return 1; });

    // ---- Controle de T e MAC
    t.add("0000000000001000", [](Cpu& c, uint16_t) -> uint32_t { set_t(c.s_, false); return 1; });
    t.add("0000000000011000", [](Cpu& c, uint16_t) -> uint32_t { set_t(c.s_, true); return 1; });
    t.add("0000000000101000", [](Cpu& c, uint16_t) -> uint32_t { c.s_.mach = c.s_.macl = 0; return 1; });
}

}  // namespace sh2
