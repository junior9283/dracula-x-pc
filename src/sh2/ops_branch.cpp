// Desvios (com e sem delay slot), chamadas/retornos, TRAPA, RTE e SLEEP.
#include "sh2/cpu.hpp"

namespace sh2 {
namespace {

constexpr uint32_t m_of(uint16_t op) { return (op >> 8) & 0xF; }
// Deslocamento de 8 e de 12 bits com sinal, em palavras de 16 bits.
constexpr int32_t disp8(uint16_t op) { return static_cast<int8_t>(op & 0xFF) * 2; }
constexpr int32_t disp12(uint16_t op) { return (static_cast<int32_t>(op & 0xFFF) << 20 >> 20) * 2; }

}  // namespace

void OpRegistrar::branch(OpcodeTable& t) {
    // ---- Desvios relativos e por registrador, todos com delay slot
    t.add("1010dddddddddddd", [](Cpu& c, uint16_t op) -> uint32_t { return 1 + c.delay_slot(c.op_address_ + 4 + disp12(op)); });
    t.add("1011dddddddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.pr = c.op_address_ + 4;
        return 1 + c.delay_slot(c.op_address_ + 4 + disp12(op));
    });
    t.add("0000mmmm00100011", [](Cpu& c, uint16_t op) -> uint32_t { return 1 + c.delay_slot(c.op_address_ + 4 + c.s_.r[m_of(op)]); });
    t.add("0000mmmm00000011", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.pr = c.op_address_ + 4;
        return 1 + c.delay_slot(c.op_address_ + 4 + c.s_.r[m_of(op)]);
    });
    t.add("0100mmmm00101011", [](Cpu& c, uint16_t op) -> uint32_t { return 1 + c.delay_slot(c.s_.r[m_of(op)]); });
    t.add("0100mmmm00001011", [](Cpu& c, uint16_t op) -> uint32_t {
        c.s_.pr = c.op_address_ + 4;
        return 1 + c.delay_slot(c.s_.r[m_of(op)]);
    });
    t.add("0000000000001011", [](Cpu& c, uint16_t) -> uint32_t { return 1 + c.delay_slot(c.s_.pr); });  // rts
    t.add("0000000000101011", [](Cpu& c, uint16_t) -> uint32_t {                                        // rte
        const uint32_t target = c.load32(c.s_.r[15]);
        c.s_.r[15] += 4;
        c.s_.sr = c.load32(c.s_.r[15]) & kSrWritable;
        c.s_.r[15] += 4;
        return 3 + c.delay_slot(target);
    });

    // ---- Desvios condicionais: sem slot (bt, bf) e com slot (bt/s, bf/s; sem salto o slot é uma instrução comum)
    t.add("10001001dddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        if (!(c.s_.sr & kSrT)) return 1;
        c.s_.pc = c.op_address_ + 4 + disp8(op);
        return 3;
    });
    t.add("10001011dddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        if (c.s_.sr & kSrT) return 1;
        c.s_.pc = c.op_address_ + 4 + disp8(op);
        return 3;
    });
    t.add("10001101dddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        return (c.s_.sr & kSrT) ? 1 + c.delay_slot(c.op_address_ + 4 + disp8(op)) : 1;
    });
    t.add("10001111dddddddd", [](Cpu& c, uint16_t op) -> uint32_t {
        return (c.s_.sr & kSrT) ? 1 : 1 + c.delay_slot(c.op_address_ + 4 + disp8(op));
    });

    // ---- TRAPA #imm e SLEEP
    t.add("11000011iiiiiiii", [](Cpu& c, uint16_t op) -> uint32_t {
        c.enter_exception(op & 0xFF, c.op_address_ + 2);
        return 8;
    });
    t.add("0000000000011011", [](Cpu& c, uint16_t) -> uint32_t { c.sleeping_ = true; return 3; });
}

}  // namespace sh2
