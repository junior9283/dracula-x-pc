// Multiplicação (MUL, MULS/MULU, DMULS/DMULU), multiplicação-acumulação (MAC.W/.L) e divisão passo a passo (DIV0S/DIV0U/DIV1).
#include "sh2/op_fields.hpp"

namespace sh2 {
namespace {

uint64_t mac_value(const Sh2State& s) { return (uint64_t(s.mach) << 32) | s.macl; }
void set_mac(Sh2State& s, uint64_t value) { s.mach = static_cast<uint32_t>(value >> 32); s.macl = static_cast<uint32_t>(value); }

}  // namespace

void OpRegistrar::mul(OpcodeTable& t) {
    t.add("0000nnnnmmmm0111", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.macl = c.s_.r[n_of(op)] * c.s_.r[m_of(op)]; return 2; });
    t.add("0010nnnnmmmm1111", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.macl = static_cast<uint32_t>(int32_t(int16_t(c.s_.r[n_of(op)])) * int32_t(int16_t(c.s_.r[m_of(op)])));
        return 1;
    });
    t.add("0010nnnnmmmm1110", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.macl = uint32_t(uint16_t(c.s_.r[n_of(op)])) * uint32_t(uint16_t(c.s_.r[m_of(op)]));
        return 1;
    });
    t.add("0011nnnnmmmm1101", [](Cpu& c, uint16_t op) -> uint32_t {
        set_mac(c.s_, static_cast<uint64_t>(int64_t(int32_t(c.s_.r[n_of(op)])) * int64_t(int32_t(c.s_.r[m_of(op)]))));
        return 2;
    });
    t.add("0011nnnnmmmm0101", [](Cpu& c, uint16_t op) -> uint32_t {
        set_mac(c.s_, uint64_t(c.s_.r[n_of(op)]) * uint64_t(c.s_.r[m_of(op)]));
        return 2;
    });

    // MAC.W @Rm+,@Rn+ : (Rn) × (Rm) + MAC; com S, soma de 32 bits saturada em MACL e MACH.bit0 marca o estouro.
    t.add("0100nnnnmmmm1111", [](Cpu& c, uint16_t op) -> uint32_t {
        const int32_t a = static_cast<int32_t>(c.load16s(c.s_.r[n_of(op)])); c.s_.r[n_of(op)] += 2;
        const int32_t b = static_cast<int32_t>(c.load16s(c.s_.r[m_of(op)])); c.s_.r[m_of(op)] += 2;
        const int64_t product = int64_t(a) * b;
        if (c.s_.sr & kSrS) {
            const int64_t sum = int64_t(int32_t(c.s_.macl)) + product;
            if (sum > INT32_MAX) { c.s_.macl = 0x7FFFFFFF; c.s_.mach |= 1; }
            else if (sum < INT32_MIN) { c.s_.macl = 0x80000000; c.s_.mach |= 1; }
            else c.s_.macl = static_cast<uint32_t>(sum);
        } else {
            set_mac(c.s_, mac_value(c.s_) + static_cast<uint64_t>(product));
        }
        return 3;
    });
    // MAC.L @Rm+,@Rn+ : acumulador de 64 bits; com S, limitado a 48 bits com sinal.
    t.add("0000nnnnmmmm1111", [](Cpu& c, uint16_t op) -> uint32_t {
        const int32_t a = static_cast<int32_t>(c.load32(c.s_.r[n_of(op)])); c.s_.r[n_of(op)] += 4;
        const int32_t b = static_cast<int32_t>(c.load32(c.s_.r[m_of(op)])); c.s_.r[m_of(op)] += 4;
        const int64_t product = int64_t(a) * b;
        if (c.s_.sr & kSrS) {
            constexpr __int128 kMax = (__int128(1) << 47) - 1, kMin = -(__int128(1) << 47);
            __int128 sum = __int128(static_cast<int64_t>(mac_value(c.s_))) + product;
            sum = sum > kMax ? kMax : (sum < kMin ? kMin : sum);
            set_mac(c.s_, static_cast<uint64_t>(static_cast<int64_t>(sum)));
        } else {
            set_mac(c.s_, mac_value(c.s_) + static_cast<uint64_t>(product));
        }
        return 3;
    });

    // ---- Divisão passo a passo
    t.add("0010nnnnmmmm0111", [](Cpu& c, uint16_t op) -> uint32_t {  // div0s Rm,Rn
        const bool q = c.s_.r[n_of(op)] >> 31, m = c.s_.r[m_of(op)] >> 31;
        set_sr_flag(c.s_, kSrQ, q); set_sr_flag(c.s_, kSrM, m); set_sr_flag(c.s_, kSrT, q != m);
        return 1;
    });
    t.add("0000000000011001", [](Cpu& c, uint16_t) -> uint32_t { c.s_.sr &= ~(kSrQ | kSrM | kSrT); return 1; });  // div0u
    t.add("0011nnnnmmmm0100", [](Cpu& c, uint16_t op) -> uint32_t {  // div1 Rm,Rn
        uint32_t& rn = c.s_.r[n_of(op)];
        const uint32_t rm = c.s_.r[m_of(op)];
        const bool old_q = c.s_.sr & kSrQ, m = c.s_.sr & kSrM, carry_in = c.s_.sr & kSrT;
        bool q = rn >> 31;
        rn = (rn << 1) | (carry_in ? 1 : 0);
        const uint32_t before = rn;
        // Subtrai quando Q antigo == M, soma caso contrário; Q novo = (carry/borrow) ^ Q do bit deslocado ^ M (os 4 casos do manual).
        const bool subtract = (old_q == m);
        rn = subtract ? rn - rm : rn + rm;
        const bool tmp1 = subtract ? rn > before : rn < before;
        q = tmp1 ^ q ^ m;
        set_sr_flag(c.s_, kSrQ, q);
        set_sr_flag(c.s_, kSrT, q == m);
        return 1;
    });
}

}  // namespace sh2
