// Instruções de transferência de dados: MOV (todas as formas), MOVA, MOVT, SWAP, XTRCT, LDC/STC/LDS/STS e NOP.
#include "sh2/op_fields.hpp"

namespace sh2 {
void OpRegistrar::data(OpcodeTable& t) {
    t.add("0000000000001001", [](Cpu&, uint16_t) -> uint32_t { return 1; });  // nop

    // ---- Imediato e entre registradores
    t.add("1110nnnniiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = static_cast<uint32_t>(simm8(op)); return 1; });
    t.add("0110nnnnmmmm0011", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.r[m_of(op)]; return 1; });

    // ---- PC-relativo (PC da instrução + 4)
    t.add("1001nnnndddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.r[n_of(op)] = c.load16s(c.op_address_ + 4 + d8_of(op) * 2);
        return 1;
    });
    t.add("1101nnnndddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.r[n_of(op)] = c.load32(((c.op_address_ + 4) & ~3u) + d8_of(op) * 4);
        return 1;
    });
    t.add("11000111dddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.r[0] = ((c.op_address_ + 4) & ~3u) + d8_of(op) * 4;
        return 1;
    });

    // ---- @Rn / @Rm
    t.add("0010nnnnmmmm0000", [](Cpu& c, uint16_t op) -> uint32_t { c.store8(c.s_.r[n_of(op)], c.s_.r[m_of(op)]); return 1; });
    t.add("0010nnnnmmmm0001", [](Cpu& c, uint16_t op) -> uint32_t { c.store16(c.s_.r[n_of(op)], c.s_.r[m_of(op)]); return 1; });
    t.add("0010nnnnmmmm0010", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)], c.s_.r[m_of(op)]); return 1; });
    t.add("0110nnnnmmmm0000", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load8s(c.s_.r[m_of(op)]); return 1; });
    t.add("0110nnnnmmmm0001", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load16s(c.s_.r[m_of(op)]); return 1; });
    t.add("0110nnnnmmmm0010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load32(c.s_.r[m_of(op)]); return 1; });

    // ---- Pré-decremento (escrita) e pós-incremento (leitura)
    t.add("0010nnnnmmmm0100", [](Cpu& c, uint16_t op) -> uint32_t { c.store8(c.s_.r[n_of(op)] - 1, c.s_.r[m_of(op)]); c.s_.r[n_of(op)] -= 1; return 1; });
    t.add("0010nnnnmmmm0101", [](Cpu& c, uint16_t op) -> uint32_t { c.store16(c.s_.r[n_of(op)] - 2, c.s_.r[m_of(op)]); c.s_.r[n_of(op)] -= 2; return 1; });
    t.add("0010nnnnmmmm0110", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.r[m_of(op)]); c.s_.r[n_of(op)] -= 4; return 1; });
    // Na leitura com Rm == Rn o valor lido prevalece sobre o pós-incremento.
    t.add("0110nnnnmmmm0100", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t value = c.load8s(c.s_.r[m_of(op)]);
        c.s_.r[m_of(op)] += 1; c.s_.r[n_of(op)] = value; return 1;
    });
    t.add("0110nnnnmmmm0101", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t value = c.load16s(c.s_.r[m_of(op)]);
        c.s_.r[m_of(op)] += 2; c.s_.r[n_of(op)] = value; return 1;
    });
    t.add("0110nnnnmmmm0110", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t value = c.load32(c.s_.r[m_of(op)]);
        c.s_.r[m_of(op)] += 4; c.s_.r[n_of(op)] = value; return 1;
    });

    // ---- Com deslocamento
    t.add("10000000nnnndddd", [](Cpu& c, uint16_t op) -> uint32_t { c.store8(c.s_.r[m_of(op)] + d4_of(op), c.s_.r[0]); return 1; });
    t.add("10000001nnnndddd", [](Cpu& c, uint16_t op) -> uint32_t { c.store16(c.s_.r[m_of(op)] + d4_of(op) * 2, c.s_.r[0]); return 1; });
    t.add("0001nnnnmmmmdddd", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] + d4_of(op) * 4, c.s_.r[m_of(op)]); return 1; });
    t.add("10000100mmmmdddd", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] = c.load8s(c.s_.r[m_of(op)] + d4_of(op)); return 1; });
    t.add("10000101mmmmdddd", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] = c.load16s(c.s_.r[m_of(op)] + d4_of(op) * 2); return 1; });
    t.add("0101nnnnmmmmdddd", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load32(c.s_.r[m_of(op)] + d4_of(op) * 4); return 1; });

    // ---- Indexado por R0
    t.add("0000nnnnmmmm0100", [](Cpu& c, uint16_t op) -> uint32_t { c.store8(c.s_.r[n_of(op)] + c.s_.r[0], c.s_.r[m_of(op)]); return 1; });
    t.add("0000nnnnmmmm0101", [](Cpu& c, uint16_t op) -> uint32_t { c.store16(c.s_.r[n_of(op)] + c.s_.r[0], c.s_.r[m_of(op)]); return 1; });
    t.add("0000nnnnmmmm0110", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] + c.s_.r[0], c.s_.r[m_of(op)]); return 1; });
    t.add("0000nnnnmmmm1100", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load8s(c.s_.r[m_of(op)] + c.s_.r[0]); return 1; });
    t.add("0000nnnnmmmm1101", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load16s(c.s_.r[m_of(op)] + c.s_.r[0]); return 1; });
    t.add("0000nnnnmmmm1110", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.load32(c.s_.r[m_of(op)] + c.s_.r[0]); return 1; });

    // ---- Relativo ao GBR
    t.add("11000000dddddddd", [](Cpu& c, uint16_t op) -> uint32_t { c.store8(c.s_.gbr + d8_of(op), c.s_.r[0]); return 1; });
    t.add("11000001dddddddd", [](Cpu& c, uint16_t op) -> uint32_t { c.store16(c.s_.gbr + d8_of(op) * 2, c.s_.r[0]); return 1; });
    t.add("11000010dddddddd", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.gbr + d8_of(op) * 4, c.s_.r[0]); return 1; });
    t.add("11000100dddddddd", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] = c.load8s(c.s_.gbr + d8_of(op)); return 1; });
    t.add("11000101dddddddd", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] = c.load16s(c.s_.gbr + d8_of(op) * 2); return 1; });
    t.add("11000110dddddddd", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[0] = c.load32(c.s_.gbr + d8_of(op) * 4); return 1; });

    // ---- MOVT, SWAP, XTRCT
    t.add("0000nnnn00101001", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = sr_flag(c.s_, kSrT) ? 1 : 0; return 1; });
    t.add("0110nnnnmmmm1000", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = c.s_.r[m_of(op)];
        c.s_.r[n_of(op)] = (m & 0xFFFF0000u) | ((m & 0xFF) << 8) | ((m >> 8) & 0xFF);
        return 1;
    });
    t.add("0110nnnnmmmm1001", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = c.s_.r[m_of(op)];
        c.s_.r[n_of(op)] = (m << 16) | (m >> 16);
        return 1;
    });
    t.add("0010nnnnmmmm1101", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.r[n_of(op)] = (c.s_.r[m_of(op)] << 16) | (c.s_.r[n_of(op)] >> 16);
        return 1;
    });

    // ---- Registradores de controle e de sistema: LDC/LDS (de registrador e de @Rm+), STC/STS (para registrador e @-Rn)
    t.add("0100mmmm00001110", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.sr = c.s_.r[n_of(op)] & kSrWritable;
        c.inhibit_irq_ = true;  // nenhuma interrupção entre o LDC SR e a instrução seguinte
        return 1;
    });
    t.add("0100mmmm00011110", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.gbr = c.s_.r[n_of(op)]; return 1; });
    t.add("0100mmmm00101110", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.vbr = c.s_.r[n_of(op)]; return 1; });
    t.add("0100mmmm00001010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.mach = c.s_.r[n_of(op)]; return 1; });
    t.add("0100mmmm00011010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.macl = c.s_.r[n_of(op)]; return 1; });
    t.add("0100mmmm00101010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.pr = c.s_.r[n_of(op)]; return 1; });

    t.add("0100mmmm00000111", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = n_of(op); const uint32_t v = c.load32(c.s_.r[m]); c.s_.r[m] += 4; c.s_.sr = v & kSrWritable;
        c.inhibit_irq_ = true; return 3;
    });
    t.add("0100mmmm00010111", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = n_of(op); const uint32_t v = c.load32(c.s_.r[m]); c.s_.r[m] += 4; c.s_.gbr = v; return 3;
    });
    t.add("0100mmmm00100111", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = n_of(op); const uint32_t v = c.load32(c.s_.r[m]); c.s_.r[m] += 4; c.s_.vbr = v; return 3;
    });
    t.add("0100mmmm00000110", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = n_of(op); const uint32_t v = c.load32(c.s_.r[m]); c.s_.r[m] += 4; c.s_.mach = v; return 1;
    });
    t.add("0100mmmm00010110", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = n_of(op); const uint32_t v = c.load32(c.s_.r[m]); c.s_.r[m] += 4; c.s_.macl = v; return 1;
    });
    t.add("0100mmmm00100110", [](Cpu& c, uint16_t op) -> uint32_t {
        const uint32_t m = n_of(op); const uint32_t v = c.load32(c.s_.r[m]); c.s_.r[m] += 4; c.s_.pr = v; return 1;
    });

    t.add("0000nnnn00000010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.sr; return 1; });
    t.add("0000nnnn00010010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.gbr; return 1; });
    t.add("0000nnnn00100010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.vbr; return 1; });
    t.add("0000nnnn00001010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.mach; return 1; });
    t.add("0000nnnn00011010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.macl; return 1; });
    t.add("0000nnnn00101010", [](Cpu& c, uint16_t op) -> uint32_t { c.s_.r[n_of(op)] = c.s_.pr; return 1; });

    t.add("0100nnnn00000011", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.sr); c.s_.r[n_of(op)] -= 4; return 2; });
    t.add("0100nnnn00010011", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.gbr); c.s_.r[n_of(op)] -= 4; return 2; });
    t.add("0100nnnn00100011", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.vbr); c.s_.r[n_of(op)] -= 4; return 2; });
    t.add("0100nnnn00000010", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.mach); c.s_.r[n_of(op)] -= 4; return 1; });
    t.add("0100nnnn00010010", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.macl); c.s_.r[n_of(op)] -= 4; return 1; });
    t.add("0100nnnn00100010", [](Cpu& c, uint16_t op) -> uint32_t { c.store32(c.s_.r[n_of(op)] - 4, c.s_.pr); c.s_.r[n_of(op)] -= 4; return 1; });
}

}  // namespace sh2
