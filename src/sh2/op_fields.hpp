#pragma once
#include "sh2/cpu.hpp"

namespace sh2 {

// Campos de um opcode do SH-2 (n = bits 8–11, m = bits 4–7, d4 = bits 0–3, d8/imm8 = bits 0–7) e acesso aos flags do SR.
constexpr uint32_t n_of(uint16_t op) { return (op >> 8) & 0xF; }
constexpr uint32_t m_of(uint16_t op) { return (op >> 4) & 0xF; }
constexpr uint32_t d4_of(uint16_t op) { return op & 0xF; }
constexpr uint32_t d8_of(uint16_t op) { return op & 0xFF; }
constexpr int32_t simm8(uint16_t op) { return static_cast<int8_t>(op & 0xFF); }

inline bool sr_flag(const Sh2State& s, uint32_t flag) { return s.sr & flag; }
inline void set_sr_flag(Sh2State& s, uint32_t flag, bool on) { s.sr = (s.sr & ~flag) | (on ? flag : 0); }

}  // namespace sh2
