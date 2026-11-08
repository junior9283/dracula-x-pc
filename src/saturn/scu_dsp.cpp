#include "saturn/scu_dsp.hpp"

#include "saturn/b_bus.hpp"

namespace saturn {
namespace {

constexpr uint64_t kMask48 = 0xFFFF'FFFF'FFFFull;
constexpr uint32_t kPpafPcLoad = 1u << 15, kPpafExecute = 1u << 16, kPpafStep = 1u << 17, kPpafPauseStart = 1u << 25,
                   kPpafPauseStop = 1u << 26;
constexpr int kCyclesPerInstruction = 2;   // ciclos do SH-2 por instrução do DSP

int64_t sext32(uint32_t v) { return static_cast<int32_t>(v); }
uint32_t sign_extend(uint32_t v, int bits) { return static_cast<uint32_t>(static_cast<int32_t>(v << (32 - bits)) >> (32 - bits)); }

// O DMA do DSP alcança o A-bus (0x02000000–0x058FFFFF), o B-bus e a Work RAM-H (até 0x07FFFFFF).
bool dsp_dma_reaches(uint32_t address) { return address >= 0x0200'0000 && !(address >= 0x0590'0000 && address < 0x05A0'0000); }

}  // namespace

uint32_t ScuDsp::read(uint32_t offset) {
    switch (offset) {
    case 0x80: {
        const uint32_t v = pc_ | uint32_t(flag_s_) << 22 | uint32_t(flag_z_) << 21 | uint32_t(flag_c_) << 20 |
                           uint32_t(flag_v_) << 19 | uint32_t(flag_end_) << 18 | uint32_t(running()) << 16;
        flag_v_ = flag_end_ = false;   // a leitura limpa V e E
        return v;
    }
    case 0x8C:
        if (running()) return 0xFFFFFFFF;
        {
            const uint8_t a = data_address_++;
            return data_[a >> 6][a & 63];
        }
    default: return 0;
    }
}

void ScuDsp::write(uint32_t offset, uint32_t value) {
    switch (offset) {
    case 0x80:
        if (value & kPpafPauseStart) paused_ = true;
        else if (value & kPpafPauseStop) paused_ = false;
        else {
            executing_ = value & kPpafExecute;
            if ((value & kPpafStep) && !executing_ && !paused_) step();
        }
        if (value & kPpafPcLoad) load_pc(static_cast<uint8_t>(value));
        break;
    case 0x84:
        if (!running()) program_[pc_++] = value;
        break;
    case 0x88: data_address_ = static_cast<uint8_t>(value); break;
    case 0x8C:
        if (!running()) {
            const uint8_t a = data_address_++;
            data_[a >> 6][a & 63] = value;
        }
        break;
    default: break;
    }
}

void ScuDsp::advance(uint32_t cycles) {
    if (!running()) { credit_ = 0; return; }
    credit_ += static_cast<int32_t>(cycles);
    while (credit_ >= kCyclesPerInstruction && running()) {
        step();
        credit_ -= kCyclesPerInstruction;
    }
}

void ScuDsp::reset() {
    ct_ = {};
    ac_ = p_ = 0;
    rx_ = ry_ = ra0_ = wa0_ = 0;
    lop_ = 0;
    top_ = 0;
    executing_ = paused_ = false;
    flag_z_ = flag_s_ = flag_c_ = flag_v_ = flag_end_ = false;
    credit_ = 0;
    load_pc(0);
}

void ScuDsp::load_pc(uint8_t pc) {
    pc_ = pc;
    next_instr_ = 0;   // a próxima execução é um NOP enquanto a instrução em `pc` é buscada
    next_looped_ = false;
    program_dma_count_ = 0;
}

// Busca adiantada: executa a instrução já buscada e busca a seguinte (salvo durante a repetição de LPS).
void ScuDsp::step() {
    const uint32_t instr = next_instr_;
    const bool looped = next_looped_;
    if (!looped || lop_ == 0) {
        next_instr_ = program_[pc_++];
        next_looped_ = false;
    }
    if (looped) lop_ = (lop_ - 1) & 0x0FFF;

    switch (instr >> 28) {
    case 0x0: case 0x1: case 0x2: case 0x3: general(instr, looped); break;
    case 0x8: case 0x9: case 0xA: case 0xB: load_immediate(instr, looped); break;
    case 0xC: dma(instr); break;
    case 0xD: case 0xE: case 0xF: special(instr); break;
    default: break;   // 0x4–0x7: NOP
    }
}

bool ScuDsp::condition(uint32_t cond) const {
    if (!(cond & 0x40)) return true;
    const bool hit = ((cond & 0x1) && flag_z_) || ((cond & 0x2) && flag_s_) || ((cond & 0x4) && flag_c_);   // bit 3 = T0, sempre 0
    return hit == bool(cond & 0x20);
}

void ScuDsp::general(uint32_t instr, bool looped) {
    const uint32_t alu_op = (instr >> 26) & 0xF, x_op = (instr >> 23) & 7, y_op = (instr >> 17) & 7, d1_op = (instr >> 12) & 3;
    const uint32_t p_low = static_cast<uint32_t>(p_);
    uint64_t alu = ac_;
    uint32_t low = static_cast<uint32_t>(alu);
    const auto flags32 = [&](uint32_t v) { flag_s_ = int32_t(v) < 0; flag_z_ = v == 0; };
    bool low_changed = true;
    switch (alu_op) {
    case 0x1: low &= p_low; flag_c_ = false; flags32(low); break;   // AND
    case 0x2: low |= p_low; flag_c_ = false; flags32(low); break;   // OR
    case 0x3: low ^= p_low; flag_c_ = false; flags32(low); break;   // XOR
    case 0x4: case 0x5: {   // ADD / SUB
        const bool sub = alu_op == 0x5;
        const uint64_t t = sub ? uint64_t(low) - p_low : uint64_t(low) + p_low;
        const uint32_t same_sign = sub ? (low ^ p_low) : ~(low ^ p_low);   // operandos que podem transbordar
        flag_v_ |= ((same_sign & (low ^ uint32_t(t))) >> 31) & 1;
        flag_c_ = (t >> 32) & 1;
        low = uint32_t(t);
        flags32(low);
        break;
    }
    case 0x6: {   // AD2 (48 bits)
        const uint64_t t = (alu & kMask48) + (p_ & kMask48);
        flag_v_ |= ((~(alu ^ p_) & (alu ^ t)) >> 47) & 1;
        flag_c_ = (t >> 48) & 1;
        flag_s_ = (t >> 47) & 1;
        flag_z_ = (t & kMask48) == 0;
        alu = t;
        low_changed = false;
        break;
    }
    case 0x8: flag_c_ = low & 1; low = uint32_t(int32_t(low) >> 1); flags32(low); break;            // SR
    case 0x9: flag_c_ = low & 1; low = (low >> 1) | (low << 31); flags32(low); break;              // RR
    case 0xA: flag_c_ = low >> 31; low <<= 1; flags32(low); break;                                  // SL
    case 0xB: flag_c_ = low >> 31; low = (low << 1) | (low >> 31); flags32(low); break;            // RL
    case 0xF: flag_c_ = (low >> 24) & 1; low = (low << 8) | (low >> 24); flags32(low); break;      // RL8
    default: low_changed = false; break;                                                            // NOP
    }
    if (low_changed) alu = (alu & ~0xFFFF'FFFFull) | low;

    uint32_t read_banks = 0;
    std::array<bool, 4> inc{};
    const auto read_source = [&](uint32_t s) {   // M0–M3 (s & 4: incrementa CT depois)
        const uint32_t bank = s & 3;
        read_banks |= 1u << bank;
        inc[bank] = inc[bank] || (s & 4);
        return data_[bank][ct_[bank]];
    };

    // Barramento X: P recebe o produto atual (RX × RY) ou o dado; RX recebe o dado.
    if ((x_op & 3) == 2) p_ = uint64_t(sext32(rx_) * sext32(ry_));
    if (x_op >= 3) {
        const uint32_t v = read_source((instr >> 20) & 7);
        if ((x_op & 3) == 3) p_ = uint64_t(sext32(v));
        if (x_op & 4) rx_ = v;
    }
    // Barramento Y: A recebe 0, a saída da ALU ou o dado; RY recebe o dado.
    if ((y_op & 3) == 1) ac_ = 0;
    else if ((y_op & 3) == 2) ac_ = alu;
    if (y_op >= 3) {
        const uint32_t v = read_source((instr >> 14) & 7);
        if ((y_op & 3) == 3) ac_ = uint64_t(sext32(v));
        if (y_op & 4) ry_ = v;
    }

    // Barramento D1: MOV SImm,[d] ou MOV [s],[d].
    if (d1_op & 1) {
        const uint32_t d = (instr >> 8) & 0xF;
        uint32_t v = sign_extend(instr & 0xFF, 8);
        if (d1_op & 2) {
            const uint32_t s = instr & 0xF;
            if (s < 8) {
                const uint32_t bank = s & 3;
                v = data_[bank][ct_[bank]];
                read_banks |= 1u << bank;
                if ((s & 4) && d != bank) inc[bank] = true;
            } else if (s == 9) v = uint32_t(alu);          // ALL
            else if (s == 0xA) v = uint32_t(alu >> 16);    // ALH
            else v = 0xFFFFFFFF;
        }
        switch (d) {
        case 0x0: case 0x1: case 0x2: case 0x3:
            if (!(read_banks & (1u << d))) { data_[d][ct_[d]] = v; inc[d] = true; }
            break;
        case 0x4: rx_ = v; break;
        case 0x5: p_ = uint64_t(sext32(v)); break;
        case 0x6: ra0_ = v; break;
        case 0x7: wa0_ = v; break;
        case 0xA: if (!looped || lop_ == 0x0FFF) lop_ = v & 0x0FFF; break;
        case 0xB: top_ = static_cast<uint8_t>(v); break;
        case 0xC: case 0xD: case 0xE: case 0xF: ct_[d - 0xC] = v & 0x3F; inc[d - 0xC] = false; break;
        default: break;
        }
    }
    for (int b = 0; b < 4; ++b) ct_[b] = (ct_[b] + inc[b]) & 0x3F;
}

void ScuDsp::load_immediate(uint32_t instr, bool looped) {
    const uint32_t dest = (instr >> 26) & 0xF, cond = (instr >> 19) & 0x7F;
    const uint32_t imm = (cond & 0x40) ? sign_extend(instr & 0x7FFFF, 19) : sign_extend(instr & 0x1FFFFFF, 25);
    if (!condition(cond)) return;
    if (program_dma_count_ && (dest == 6 || dest == 7)) {
        --pc_;
        finish_program_dma();
    }
    switch (dest) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        data_[dest][ct_[dest]] = imm;
        ct_[dest] = (ct_[dest] + 1) & 0x3F;
        break;
    case 0x4: rx_ = imm; break;
    case 0x5: p_ = uint64_t(sext32(imm)); break;
    case 0x6: ra0_ = imm; break;
    case 0x7: wa0_ = imm; break;
    case 0xA: if (!looped || lop_ == 0x0FFF) lop_ = imm & 0x0FFF; break;
    case 0xC:   // salto com retorno: TOP guarda o endereço de volta
        top_ = static_cast<uint8_t>(pc_ - 1);
        pc_ = static_cast<uint8_t>(imm);
        if (program_dma_count_) finish_program_dma();
        break;
    default: break;
    }
}

void ScuDsp::dma(uint32_t instr) {
    const uint32_t add_mode = (instr >> 15) & 7, ram = (instr >> 8) & 7;
    const bool hold = instr & (1u << 14), from_ram_count = instr & (1u << 13), to_bus = instr & (1u << 12);
    uint32_t count;
    if (from_ram_count) {
        const uint32_t bank = instr & 3;
        count = data_[bank][ct_[bank]] & 0xFF;
        if (instr & 4) ct_[bank] = (ct_[bank] + 1) & 0x3F;
    } else {
        count = instr & 0xFF;
    }
    if (count == 0) count = 256;

    if (to_bus) {   // RAM de dados → barramento (ram 4–7: escreve 0xFFFFFFFF)
        const uint32_t step = (1u << add_mode) & ~1u;
        uint32_t address = (wa0_ << 2) & 0x07FFFFFF;
        if (!dsp_dma_reaches(address)) return;
        const bool b_bus = is_b_bus(address);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t v = 0xFFFFFFFF;
            if (!(ram & 4)) {
                v = data_[ram][ct_[ram]];
                ct_[ram] = (ct_[ram] + 1) & 0x3F;
            }
            if (b_bus) address = write_b_bus_long(bus_, address, v, step);
            else bus_.write32(address & ~3u, v);
            address += step;
        }
        if (!hold) wa0_ = (address + 2) >> 2;
    } else {        // barramento → RAM de dados (ram 0–3) ou RAM de programa (ram 4)
        const uint32_t step = (1u << (add_mode & 2)) & ~1u;
        uint32_t address = (ra0_ << 2) & 0x07FFFFFF;
        if (!dsp_dma_reaches(address)) return;
        const bool b_bus = is_b_bus(address);
        program_dma_count_ = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t v;
            if (b_bus) {
                v = uint32_t(bus_.read16(address)) << 16 | bus_.read16(address | 2);
                address += 4;
            } else {
                v = bus_.read32(address & ~3u);
                address += step;
            }
            if (ram & 4) {
                if (!(ram & 3)) program_dma_[program_dma_count_++ & 0xFF] = v;
            } else {
                data_[ram][ct_[ram]] = v;
                ct_[ram] = (ct_[ram] + 1) & 0x3F;
            }
        }
        if (!hold) ra0_ = address >> 2;
    }
}

void ScuDsp::special(uint32_t instr) {
    if ((instr >> 28) == 0xD) {   // JMP (condicional): o salto vale depois da instrução já buscada
        if (condition((instr >> 19) & 0x7F)) pc_ = static_cast<uint8_t>(instr);
        return;
    }
    switch ((instr >> 27) & 3) {
    case 0:   // BTM
        if (lop_) pc_ = top_;
        lop_ = (lop_ - 1) & 0x0FFF;
        break;
    case 1:   // LPS: repete a instrução seguinte LOP+1 vezes
        next_looped_ = true;
        break;
    default:  // END / ENDI
        if (instr & (1u << 27)) {
            flag_end_ = true;
            if (on_end_interrupt) on_end_interrupt();
        }
        if (program_dma_count_) finish_program_dma();
        else executing_ = false;
        break;
    }
}

// O DMA para a RAM de programa só entra em vigor no fim do programa: grava a partir do PC atual e reinicia em TOP.
void ScuDsp::finish_program_dma() {
    for (uint32_t i = 0; i < program_dma_count_; ++i) program_[pc_++] = program_dma_[i & 0xFF];
    load_pc(top_);
}

void ScuDsp::save(StateWriter& w) const {
    for (uint32_t v : program_) w.u32(v);
    for (const auto& bank : data_)
        for (uint32_t v : bank) w.u32(v);
    for (uint8_t v : ct_) w.u8(v);
    w.u64(ac_);
    w.u64(p_);
    for (uint32_t v : {rx_, ry_, ra0_, wa0_, next_instr_}) w.u32(v);
    w.u16(lop_);
    for (uint8_t v : {pc_, top_, data_address_}) w.u8(v);
    for (bool v : {next_looped_, executing_, paused_, flag_z_, flag_s_, flag_c_, flag_v_, flag_end_}) w.boolean(v);
    w.u32(static_cast<uint32_t>(credit_));
    for (uint32_t v : program_dma_) w.u32(v);
    w.u32(program_dma_count_);
}

void ScuDsp::load(StateReader& r) {
    for (uint32_t& v : program_) v = r.u32();
    for (auto& bank : data_)
        for (uint32_t& v : bank) v = r.u32();
    for (uint8_t& v : ct_) {   // índice direto nas RAMs de dados: um valor fora de 0..63 sairia do array
        v = r.u8();
        if (v >= 64) throw StateError("save state corrompido: CT do DSP inválido");
    }
    ac_ = r.u64();
    p_ = r.u64();
    for (uint32_t* v : {&rx_, &ry_, &ra0_, &wa0_, &next_instr_}) *v = r.u32();
    lop_ = r.u16();
    for (uint8_t* v : {&pc_, &top_, &data_address_}) *v = r.u8();
    for (bool* v : {&next_looped_, &executing_, &paused_, &flag_z_, &flag_s_, &flag_c_, &flag_v_, &flag_end_}) *v = r.boolean();
    credit_ = static_cast<int32_t>(r.u32());
    for (uint32_t& v : program_dma_) v = r.u32();
    program_dma_count_ = r.u32();
    if (program_dma_count_ > program_dma_.size()) throw StateError("save state corrompido: contagem de DMA de programa inválida");
}

}  // namespace saturn
