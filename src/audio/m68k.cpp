#include "audio/m68k.hpp"

namespace audio {

namespace {

constexpr uint32_t mask_of(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
constexpr uint32_t msb_of(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }
constexpr int32_t sext(uint32_t v, int sz) {
    return sz == 1 ? static_cast<int8_t>(v) : sz == 2 ? static_cast<int16_t>(v) : static_cast<int32_t>(v);
}
// Campo de tamanho de 2 bits (0,1,2) -> bytes (1,2,4).
constexpr int size_of(int bits) { return 1 << bits; }

// Excecao interna: abandona a instrucao e vai para o vetor indicado.
struct Fault {
    int vec;
    bool at_start;  // true: empilha o PC da instrucao; false: o PC ja avancado
};

// Modos de enderecamento permitidos (um bit por modo; ver Ea::ea()).
// bits: 0 Dn, 1 An, 2 (An), 3 (An)+, 4 -(An), 5 d16(An), 6 d8(An,Xn),
//       7 abs.W, 8 abs.L, 9 d16(PC), 10 d8(PC,Xn), 11 #imm
constexpr unsigned kAll = 0xFFF;
constexpr unsigned kData = 0xFFD;      // tudo menos An
constexpr unsigned kDataNoImm = 0x7FD;  // BTST estatico
constexpr unsigned kDataAlt = 0x1FD;   // dados alteraveis
constexpr unsigned kMemAlt = 0x1FC;    // memoria alteravel
constexpr unsigned kAlt = 0x1FF;       // alteraveis (inclui An)
constexpr unsigned kControl = 0x7E4;   // modos de controle (LEA, JMP...)
constexpr unsigned kMovemStore = 0x1F4;  // (An), -(An), d16, d8, abs
constexpr unsigned kMovemLoad = 0x7EC;   // (An), (An)+, d16, d8, abs, PC

[[noreturn]] void illegal() { throw Fault{4, true}; }

}  // namespace

// ---------------------------------------------------------------- barramento

uint32_t M68k::rd(uint32_t addr, int sz) {
    addr &= 0xFFFFFF;
    if (sz == 1) return bus_.read8(addr);
    addr &= ~1u;
    uint32_t v = bus_.read16(addr);
    if (sz == 4) v = (v << 16) | bus_.read16((addr + 2) & 0xFFFFFF);
    return v;
}

void M68k::wr(uint32_t addr, int sz, uint32_t v) {
    addr &= 0xFFFFFF;
    if (sz == 1) { bus_.write8(addr, static_cast<uint8_t>(v)); return; }
    addr &= ~1u;
    if (sz == 4) {
        bus_.write16(addr, static_cast<uint16_t>(v >> 16));
        bus_.write16((addr + 2) & 0xFFFFFF, static_cast<uint16_t>(v));
    } else {
        bus_.write16(addr, static_cast<uint16_t>(v));
    }
}

uint16_t M68k::fetch16() {
    uint16_t v = static_cast<uint16_t>(rd(pc, 2));
    pc = (pc + 2) & 0xFFFFFF;
    return v;
}

uint32_t M68k::fetch32() {
    uint32_t hi = fetch16();
    return (hi << 16) | fetch16();
}

void M68k::push32(uint32_t v) { a[7] -= 4; wr(a[7], 4, v); }
uint32_t M68k::pop32() { uint32_t v = rd(a[7], 4); a[7] += 4; return v; }
uint16_t M68k::pop16() { uint16_t v = static_cast<uint16_t>(rd(a[7], 2)); a[7] += 2; return v; }

// ------------------------------------------------------------ estado / SR

void M68k::set_sr(uint16_t v) {
    v &= 0xA71F;
    if ((sr_ ^ v) & kSuper) std::swap(a[7], other_sp_);
    sr_ = v;
}

void M68k::require_super() {
    if (!(sr_ & kSuper)) throw Fault{8, true};
}

void M68k::reset() {
    for (auto& r : d) r = 0;
    for (auto& r : a) r = 0;
    sr_ = 0x2700;
    other_sp_ = 0;
    pending_ = 0;
    stopped_ = false;
    a[7] = rd(0, 4);
    pc = rd(4, 4) & 0xFFFFFF;
}

void M68k::save(saturn::StateWriter& w) const {
    for (uint32_t v : d) w.u32(v);
    for (uint32_t v : a) w.u32(v);
    w.u32(pc);
    w.u16(sr_);
    w.u32(other_sp_);
    w.i32(pending_);
    w.boolean(stopped_);
}

void M68k::load(saturn::StateReader& r) {
    for (uint32_t& v : d) v = r.u32();
    for (uint32_t& v : a) v = r.u32();
    pc = r.u32();
    sr_ = r.u16();
    other_sp_ = r.u32();
    pending_ = r.i32();
    stopped_ = r.boolean();
}

void M68k::raise_interrupt(int level) {
    if (level > pending_) pending_ = level > 7 ? 7 : level;
}

void M68k::exception(int vec, uint32_t push_pc) {
    uint16_t old = sr_;
    set_sr(static_cast<uint16_t>((sr_ | kSuper) & ~kTrace));
    push32(push_pc);
    a[7] -= 2;
    wr(a[7], 2, old);
    pc = rd(static_cast<uint32_t>(vec) * 4, 4) & 0xFFFFFF;
    cyc_ += 34;
}

int M68k::step() {
    cyc_ = 0;
    if (pending_ > ((sr_ & kIntMask) >> 8) || pending_ == 7) {
        int lvl = pending_;
        pending_ = 0;
        stopped_ = false;
        exception(24 + lvl, pc);
        sr_ = static_cast<uint16_t>((sr_ & ~kIntMask) | (lvl << 8));
        return cyc_ + 10;
    }
    if (stopped_) return 4;

    ipc_ = pc;
    const bool trace = sr_ & kTrace;
    try {
        op_ = fetch16();
        exec();
        if (trace) exception(9, pc);
    } catch (const Fault& f) {
        exception(f.vec, f.at_start ? ipc_ : pc);
    }
    return cyc_;
}

// --------------------------------------------------------------- operandos

int32_t M68k::index_ext() {
    uint16_t x = fetch16();
    int32_t idx = static_cast<int32_t>(gpr(((x >> 12) & 7) + ((x & 0x8000) ? 8 : 0)));
    if (!(x & 0x800)) idx = static_cast<int16_t>(idx);
    return idx + static_cast<int8_t>(x);
}

M68k::Ea M68k::ea(int mode, int reg, int sz, unsigned allowed) {
    const int bit = mode < 7 ? mode : (reg < 5 ? 7 + reg : -1);
    if (bit < 0 || !((allowed >> bit) & 1) || (mode == 1 && sz == 1)) illegal();
    const bool l = sz == 4;
    Ea e;
    switch (mode) {
        case 0: e.kind = Ea::Dreg; e.reg = reg; break;
        case 1: e.kind = Ea::Areg; e.reg = reg; break;
        case 2: e.addr = a[reg]; cyc_ += l ? 8 : 4; break;
        case 3:
            e.addr = a[reg];
            a[reg] += (sz == 1 && reg == 7) ? 2 : sz;
            cyc_ += l ? 8 : 4;
            break;
        case 4:
            a[reg] -= (sz == 1 && reg == 7) ? 2 : sz;
            e.addr = a[reg];
            cyc_ += l ? 10 : 6;
            break;
        case 5: e.addr = a[reg] + static_cast<int16_t>(fetch16()); cyc_ += l ? 12 : 8; break;
        case 6: { int32_t i = index_ext(); e.addr = a[reg] + i; cyc_ += l ? 14 : 10; break; }
        default:
            switch (reg) {
                case 0: e.addr = static_cast<uint32_t>(static_cast<int16_t>(fetch16())); cyc_ += l ? 12 : 8; break;
                case 1: e.addr = fetch32(); cyc_ += l ? 16 : 12; break;
                case 2: { uint32_t base = pc; e.addr = base + static_cast<int16_t>(fetch16()); cyc_ += l ? 12 : 8; break; }
                case 3: { uint32_t base = pc; e.addr = base + index_ext(); cyc_ += l ? 14 : 10; break; }
                default:
                    e.kind = Ea::Imm;
                    e.addr = l ? fetch32() : (fetch16() & mask_of(sz));
                    cyc_ += l ? 8 : 4;
                    break;
            }
    }
    return e;
}

uint32_t M68k::ea_read(const Ea& e, int sz) {
    switch (e.kind) {
        case Ea::Dreg: return d[e.reg] & mask_of(sz);
        case Ea::Areg: return a[e.reg] & mask_of(sz);
        case Ea::Imm: return e.addr;
        default: return rd(e.addr, sz);
    }
}

void M68k::ea_write(const Ea& e, int sz, uint32_t v) {
    switch (e.kind) {
        case Ea::Dreg: d[e.reg] = (d[e.reg] & ~mask_of(sz)) | (v & mask_of(sz)); break;
        case Ea::Areg: a[e.reg] = v; break;
        case Ea::Imm: break;
        default: wr(e.addr, sz, v);
    }
}

// ------------------------------------------------------------ flags e ALU

bool M68k::cond(int cc) const {
    const bool c = sr_ & kC, v = sr_ & kV, z = sr_ & kZ, n = sr_ & kN;
    switch (cc) {
        case 0: return true;
        case 1: return false;
        case 2: return !c && !z;
        case 3: return c || z;
        case 4: return !c;
        case 5: return c;
        case 6: return !z;
        case 7: return z;
        case 8: return !v;
        case 9: return v;
        case 10: return !n;
        case 11: return n;
        case 12: return n == v;
        case 13: return n != v;
        case 14: return !z && n == v;
        default: return z || n != v;
    }
}

void M68k::set_nz(uint32_t r, int sz) {
    r &= mask_of(sz);
    uint16_t f = sr_ & ~(kN | kZ | kV | kC);
    if (r & msb_of(sz)) f |= kN;
    if (r == 0) f |= kZ;
    sr_ = f;
}

uint32_t M68k::alu(Alu op, uint32_t dst, uint32_t src, int sz, bool use_x) {
    const uint32_t m = mask_of(sz), s = msb_of(sz);
    dst &= m;
    src &= m;
    const uint32_t xin = (use_x && (sr_ & kX)) ? 1 : 0;
    uint32_t res;
    bool c = false, v = false;
    switch (op) {
        case Alu::Add: {
            uint64_t t = uint64_t{dst} + src + xin;
            res = static_cast<uint32_t>(t) & m;
            c = t > m;
            v = ((src ^ res) & (dst ^ res) & s) != 0;
            break;
        }
        case Alu::Sub:
        case Alu::Cmp:
            res = (dst - src - xin) & m;
            c = uint64_t{dst} < uint64_t{src} + xin;
            v = ((src ^ dst) & (dst ^ res) & s) != 0;
            break;
        case Alu::And: res = dst & src; break;
        case Alu::Or: res = dst | src; break;
        default: res = dst ^ src; break;
    }
    uint16_t f = sr_ & ~(kN | kV | kC);
    if (use_x) {
        if (res != 0) f &= ~kZ;  // ADDX/SUBX/NEGX: Z so e limpo, nunca setado
    } else {
        f = (f & ~kZ) | (res == 0 ? kZ : 0);
    }
    if (res & s) f |= kN;
    if (v) f |= kV;
    if (c) f |= kC;
    if (op == Alu::Add || op == Alu::Sub) f = static_cast<uint16_t>((f & ~kX) | (c ? kX : 0));
    sr_ = f;
    return res;
}

uint32_t M68k::shift(int type, bool left, uint32_t v, int cnt, int sz) {
    const uint32_t m = mask_of(sz), s = msb_of(sz);
    v &= m;
    bool x = sr_ & kX, c = (cnt == 0) ? (type == 2 && x) : false, ovf = false;
    for (int i = 0; i < cnt; ++i) {
        const uint32_t before = v;
        if (left) {
            c = v & s;
            v = (v << 1) & m;
            if (type == 3) v |= c ? 1 : 0;
            else if (type == 2) v |= x ? 1 : 0;
            if (type == 0 && ((before ^ v) & s)) ovf = true;
        } else {
            c = v & 1;
            v >>= 1;
            if (type == 0) v |= before & s;
            else if (type == 3) v |= c ? s : 0;
            else if (type == 2) v |= x ? s : 0;
        }
        if (type == 2) x = c;
    }
    uint16_t f = sr_ & ~(kN | kZ | kV | kC);
    if (v & s) f |= kN;
    if (v == 0) f |= kZ;
    if (ovf) f |= kV;
    if (c) f |= kC;
    if (cnt > 0 && type != 3) f = static_cast<uint16_t>((f & ~kX) | (c ? kX : 0));
    sr_ = f;
    return v;
}

uint8_t M68k::bcd(bool sub, uint8_t dst, uint8_t src) {
    const int x = (sr_ & kX) ? 1 : 0;
    int lo, hi;
    bool carry = false;
    if (!sub) {
        lo = (dst & 0xF) + (src & 0xF) + x;
        hi = (dst >> 4) + (src >> 4);
        if (lo > 9) { lo -= 10; ++hi; }
        if (hi > 9) { hi -= 10; carry = true; }
    } else {
        lo = (dst & 0xF) - (src & 0xF) - x;
        hi = (dst >> 4) - (src >> 4);
        if (lo < 0) { lo += 10; --hi; }
        if (hi < 0) { hi += 10; carry = true; }
    }
    const uint8_t res = static_cast<uint8_t>(((hi & 0xF) << 4) | (lo & 0xF));
    uint16_t f = sr_ & ~(kN | kV | kC | kX);
    if (res != 0) f &= ~kZ;
    if (res & 0x80) f |= kN;
    if (carry) f |= kC | kX;
    sr_ = f;
    return res;
}

// ------------------------------------------------------------- decodificacao

void M68k::exec() {
    switch (op_ >> 12) {
        case 0x0: grp0(); break;
        case 0x1: case 0x2: case 0x3: grp_move(); break;
        case 0x4: grp4(); break;
        case 0x5: grp5(); break;
        case 0x6: grp6(); break;
        case 0x7:
            if (op_ & 0x100) illegal();
            d[(op_ >> 9) & 7] = static_cast<uint32_t>(static_cast<int8_t>(op_));
            set_nz(d[(op_ >> 9) & 7], 4);
            cyc_ += 4;
            break;
        case 0x8: grp8(); break;
        case 0x9: grp_addsub(false); break;
        case 0xA: throw Fault{10, true};
        case 0xB: grpB(); break;
        case 0xC: grpC(); break;
        case 0xD: grp_addsub(true); break;
        case 0xE: grpE(); break;
        default: throw Fault{11, true};
    }
}

// Operacoes diadicas OR/AND/ADD/SUB: <ea>,Dn (opm 0..2) ou Dn,<ea> (opm 4..6).
void M68k::dyadic(Alu op) {
    const int opm = (op_ >> 6) & 7, dn = (op_ >> 9) & 7, mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (opm == 3 || opm == 7) illegal();
    const int sz = size_of(opm & 3);
    const bool arith = op == Alu::Add || op == Alu::Sub;
    if (opm < 3) {
        cyc_ += sz == 4 ? 6 : 4;
        Ea e = ea(mode, reg, sz, arith ? kAll : kData);
        uint32_t r = alu(op, d[dn], ea_read(e, sz), sz);
        d[dn] = (d[dn] & ~mask_of(sz)) | (r & mask_of(sz));
    } else {
        cyc_ += sz == 4 ? 12 : 8;
        Ea e = ea(mode, reg, sz, kMemAlt);
        ea_write(e, sz, alu(op, ea_read(e, sz), d[dn], sz));
    }
}

void M68k::grp_move() {
    const int hi = op_ >> 12;
    const int sz = hi == 1 ? 1 : hi == 3 ? 2 : 4;
    const int dmode = (op_ >> 6) & 7, dreg = (op_ >> 9) & 7;
    cyc_ += 4;
    uint32_t v = ea_read(ea((op_ >> 3) & 7, op_ & 7, sz, kAll), sz);
    if (dmode == 1) {  // MOVEA: sem flags, estende o sinal
        if (sz == 1) illegal();
        a[dreg] = static_cast<uint32_t>(sext(v, sz));
        return;
    }
    Ea dst = ea(dmode, dreg, sz, kDataAlt);
    ea_write(dst, sz, v);
    set_nz(v, sz);
}

void M68k::bitop(int type, uint32_t bit, int mode, int reg, bool dynamic) {
    // type: 0 BTST, 1 BCHG, 2 BCLR, 3 BSET
    Ea e = ea(mode, reg, mode == 0 ? 4 : 1, type == 0 ? (dynamic ? kData : kDataNoImm) : kDataAlt);
    const int sz = mode == 0 ? 4 : 1;
    const uint32_t b = uint32_t{1} << (bit & (mode == 0 ? 31 : 7));
    uint32_t v = ea_read(e, sz);
    sr_ = static_cast<uint16_t>((sr_ & ~kZ) | ((v & b) ? 0 : kZ));
    cyc_ += type == 0 ? 4 : 8;
    if (type == 0) return;
    v = type == 1 ? (v ^ b) : type == 2 ? (v & ~b) : (v | b);
    ea_write(e, sz, v);
}

void M68k::movep() {
    const int dn = (op_ >> 9) & 7, opm = (op_ >> 6) & 7;
    uint32_t addr = a[op_ & 7] + static_cast<int16_t>(fetch16());
    const int n = (opm & 1) ? 4 : 2;
    cyc_ += 16 + (n == 4 ? 8 : 0);
    if (opm < 6) {  // memoria -> registrador
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | rd(addr + 2 * i, 1);
        d[dn] = n == 4 ? v : (d[dn] & 0xFFFF0000u) | v;
    } else {        // registrador -> memoria
        for (int i = 0; i < n; ++i) wr(addr + 2 * i, 1, d[dn] >> (8 * (n - 1 - i)));
    }
}

// ORI/ANDI/EORI para CCR (byte) ou SR (palavra, privilegiado). kind: 0 OR, 1 AND, 2 EOR.
void M68k::imm_to_ccr_sr(int kind) {
    const bool to_sr = (op_ & 0x40) != 0;
    if (to_sr) require_super();
    const uint16_t imm = to_sr ? fetch16() : (fetch16() & 0xFF);
    const uint16_t cur = to_sr ? sr_ : (sr_ & 0xFF);
    const uint16_t r = kind == 0 ? (cur | imm) : kind == 1 ? (cur & imm) : (cur ^ imm);
    set_sr(to_sr ? r : static_cast<uint16_t>((sr_ & 0xFF00) | (r & 0x1F)));
    cyc_ += 20;
}

void M68k::grp0() {
    const int mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (op_ & 0x100) {
        if (mode == 1) { movep(); return; }
        bitop((op_ >> 6) & 3, d[(op_ >> 9) & 7], mode, reg, true);
        return;
    }
    const int sel = (op_ >> 9) & 7;
    if (sel == 4) {
        uint32_t bit = fetch16();
        bitop((op_ >> 6) & 3, bit, mode, reg, false);
        return;
    }
    if (sel == 7) illegal();
    if ((op_ & 0xBF) == 0x3C && (sel == 0 || sel == 1 || sel == 5)) {  // x,CCR / x,SR
        imm_to_ccr_sr(sel == 0 ? 0 : sel == 1 ? 1 : 2);
        return;
    }
    if (((op_ >> 6) & 3) == 3) illegal();
    const int sz = size_of((op_ >> 6) & 3);
    uint32_t imm = sz == 4 ? fetch32() : (fetch16() & mask_of(sz));
    Alu op;
    switch (sel) {
        case 0: op = Alu::Or; break;
        case 1: op = Alu::And; break;
        case 2: op = Alu::Sub; break;
        case 3: op = Alu::Add; break;
        case 5: op = Alu::Eor; break;
        default: op = Alu::Cmp; break;
    }
    cyc_ += sz == 4 ? 16 : 8;
    Ea e = ea(mode, reg, sz, kDataAlt);
    uint32_t r = alu(op, ea_read(e, sz), imm, sz);
    if (op != Alu::Cmp) ea_write(e, sz, r);
}

void M68k::movem() {
    const bool to_reg = op_ & 0x400;
    const int sz = (op_ & 0x40) ? 4 : 2, mode = (op_ >> 3) & 7, reg = op_ & 7;
    const uint16_t mask = fetch16();
    int n = 0;
    cyc_ += 8;
    if (!to_reg && mode == 4) {  // predecremento: mascara invertida (bit0 = A7)
        uint32_t addr = a[reg];
        uint32_t snapshot[16];
        for (int i = 0; i < 16; ++i) snapshot[i] = gpr(i);
        for (int i = 0; i < 16; ++i) {
            if (!(mask >> i & 1)) continue;
            addr -= sz;
            wr(addr, sz, snapshot[15 - i]);
            ++n;
        }
        a[reg] = addr;
    } else {
        uint32_t addr;
        if (to_reg && mode == 3) addr = a[reg];
        else addr = ea(mode, reg, sz, to_reg ? kMovemLoad : kMovemStore).addr;
        for (int i = 0; i < 16; ++i) {
            if (!(mask >> i & 1)) continue;
            if (to_reg) gpr(i) = static_cast<uint32_t>(sext(rd(addr, sz), sz));
            else wr(addr, sz, gpr(i));
            addr += sz;
            ++n;
        }
        if (to_reg && mode == 3) a[reg] = addr;
    }
    cyc_ += n * (sz == 4 ? 8 : 4);
}

void M68k::multiply(bool is_signed) {
    const int dn = (op_ >> 9) & 7;
    cyc_ += 70;
    uint32_t src = ea_read(ea((op_ >> 3) & 7, op_ & 7, 2, kData), 2);
    uint32_t r = is_signed
        ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(d[dn])) * static_cast<int16_t>(src))
        : (d[dn] & 0xFFFF) * src;
    d[dn] = r;
    set_nz(r, 4);
}

void M68k::divide(bool is_signed) {
    const int dn = (op_ >> 9) & 7;
    cyc_ += is_signed ? 158 : 140;
    uint32_t src = ea_read(ea((op_ >> 3) & 7, op_ & 7, 2, kData), 2);
    if (src == 0) throw Fault{5, false};
    int64_t q, r;
    if (is_signed) {
        int64_t dv = static_cast<int32_t>(d[dn]), sv = static_cast<int16_t>(src);
        q = dv / sv;
        r = dv % sv;
    } else {
        q = d[dn] / src;
        r = d[dn] % src;
    }
    const bool overflow = is_signed ? (q > 32767 || q < -32768) : (q > 0xFFFF);
    if (overflow) {  // operando destino inalterado
        sr_ = static_cast<uint16_t>((sr_ & ~(kN | kZ | kC)) | kV | kN);
        return;
    }
    d[dn] = (static_cast<uint32_t>(r & 0xFFFF) << 16) | static_cast<uint32_t>(q & 0xFFFF);
    set_nz(static_cast<uint32_t>(q), 2);
}

// ABCD/SBCD: Dy,Dx ou -(Ay),-(Ax).
void M68k::bcd_op(bool sub) {
    const int rx = (op_ >> 9) & 7, ry = op_ & 7;
    cyc_ += 6;
    if (op_ & 8) {
        Ea s = ea(4, ry, 1, kAll), t = ea(4, rx, 1, kAll);
        ea_write(t, 1, bcd(sub, static_cast<uint8_t>(ea_read(t, 1)), static_cast<uint8_t>(ea_read(s, 1))));
    } else {
        d[rx] = (d[rx] & ~0xFFu) | bcd(sub, static_cast<uint8_t>(d[rx]), static_cast<uint8_t>(d[ry]));
    }
}

void M68k::misc_no_operand() {
    cyc_ += 4;
    switch (op_) {
        case 0x4AFC: illegal();
        case 0x4E70: require_super(); cyc_ += 128; break;  // RESET
        case 0x4E71: break;                                 // NOP
        case 0x4E72: require_super(); set_sr(fetch16()); stopped_ = true; break;  // STOP
        case 0x4E73: {                                      // RTE
            require_super();
            uint16_t s = pop16();
            pc = pop32() & 0xFFFFFF;
            set_sr(s);
            cyc_ += 16;
            break;
        }
        case 0x4E75: pc = pop32() & 0xFFFFFF; cyc_ += 12; break;  // RTS
        case 0x4E76: if (sr_ & kV) throw Fault{7, false}; break;  // TRAPV
        default: {                                                // RTR
            uint16_t s = pop16();
            pc = pop32() & 0xFFFFFF;
            set_sr(static_cast<uint16_t>((sr_ & 0xFFE0) | (s & 0x1F)));
            cyc_ += 16;
        }
    }
}

void M68k::grp4() {
    const int mode = (op_ >> 3) & 7, reg = op_ & 7, dn = (op_ >> 9) & 7;
    if (op_ == 0x4AFC || (op_ >= 0x4E70 && op_ <= 0x4E77 && op_ != 0x4E74)) { misc_no_operand(); return; }
    if ((op_ & 0xFFF0) == 0x4E40) throw Fault{32 + (op_ & 15), false};  // TRAP
    if ((op_ & 0xFFF8) == 0x4E50) {  // LINK
        push32(a[reg]);
        a[reg] = a[7];
        a[7] += static_cast<int16_t>(fetch16());
        cyc_ += 16;
        return;
    }
    if ((op_ & 0xFFF8) == 0x4E58) { a[7] = a[reg]; a[reg] = pop32(); cyc_ += 12; return; }  // UNLK
    if ((op_ & 0xFFF0) == 0x4E60) {  // MOVE USP
        require_super();
        if (op_ & 8) a[reg] = usp();
        else set_usp(a[reg]);
        cyc_ += 4;
        return;
    }
    if ((op_ & 0xFF80) == 0x4E80) {  // JSR / JMP
        Ea e = ea(mode, reg, 4, kControl);
        if (!(op_ & 0x40)) push32(pc);
        pc = e.addr;
        cyc_ += 8;
        return;
    }
    if ((op_ & 0xF1C0) == 0x41C0) {  // LEA
        a[dn] = ea(mode, reg, 4, kControl).addr;
        cyc_ += 4;
        return;
    }
    if ((op_ & 0xF1C0) == 0x4180) {  // CHK
        cyc_ += 10;
        int16_t bound = static_cast<int16_t>(ea_read(ea(mode, reg, 2, kData), 2));
        int16_t v = static_cast<int16_t>(d[dn]);
        if (v < 0 || v > bound) {
            sr_ = static_cast<uint16_t>((sr_ & ~kN) | (v < 0 ? kN : 0));
            throw Fault{6, false};
        }
        return;
    }
    if ((op_ & 0xFB80) == 0x4880 && mode != 0) { movem(); return; }
    if ((op_ & 0xFFB8) == 0x4880) {  // EXT.W / EXT.L
        const bool l = op_ & 0x40;
        d[reg] = l ? static_cast<uint32_t>(static_cast<int16_t>(d[reg]))
                   : (d[reg] & 0xFFFF0000u) | static_cast<uint16_t>(static_cast<int8_t>(d[reg]));
        set_nz(d[reg], l ? 4 : 2);
        cyc_ += 4;
        return;
    }
    if ((op_ & 0xFFF8) == 0x4840) {  // SWAP
        d[reg] = (d[reg] << 16) | (d[reg] >> 16);
        set_nz(d[reg], 4);
        cyc_ += 4;
        return;
    }
    if ((op_ & 0xFFC0) == 0x4840) {  // PEA
        push32(ea(mode, reg, 4, kControl).addr);
        cyc_ += 12;
        return;
    }
    switch (op_ & 0xFFC0) {
        case 0x40C0: ea_write(ea(mode, reg, 2, kDataAlt), 2, sr_); cyc_ += 6; return;  // MOVE SR,<ea>
        case 0x44C0:  // MOVE <ea>,CCR
            set_sr(static_cast<uint16_t>((sr_ & 0xFF00) | (ea_read(ea(mode, reg, 2, kData), 2) & 0x1F)));
            cyc_ += 12;
            return;
        case 0x46C0:  // MOVE <ea>,SR
            require_super();
            set_sr(static_cast<uint16_t>(ea_read(ea(mode, reg, 2, kData), 2)));
            cyc_ += 12;
            return;
        case 0x4800: {  // NBCD
            Ea e = ea(mode, reg, 1, kDataAlt);
            ea_write(e, 1, bcd(true, 0, static_cast<uint8_t>(ea_read(e, 1))));
            cyc_ += 6;
            return;
        }
        case 0x4AC0: {  // TAS
            Ea e = ea(mode, reg, 1, kDataAlt);
            uint32_t v = ea_read(e, 1);
            set_nz(v, 1);
            ea_write(e, 1, v | 0x80);
            cyc_ += 10;
            return;
        }
        default: break;
    }
    // NEGX / CLR / NEG / NOT / TST
    const int sel = (op_ >> 8) & 0xF, sz_bits = (op_ >> 6) & 3;
    if (sz_bits == 3 || (sel != 0 && sel != 2 && sel != 4 && sel != 6 && sel != 0xA)) illegal();
    const int sz = size_of(sz_bits);
    Ea e = ea(mode, reg, sz, kDataAlt);
    cyc_ += mode == 0 ? (sz == 4 ? 6 : 4) : (sz == 4 ? 12 : 8);
    switch (sel) {
        case 0: ea_write(e, sz, alu(Alu::Sub, 0, ea_read(e, sz), sz, true)); break;
        case 2: ea_write(e, sz, 0); set_nz(0, sz); break;
        case 4: ea_write(e, sz, alu(Alu::Sub, 0, ea_read(e, sz), sz)); break;
        case 6: { uint32_t r = ~ea_read(e, sz); ea_write(e, sz, r); set_nz(r, sz); break; }
        default: set_nz(ea_read(e, sz), sz); break;
    }
}

void M68k::grp5() {
    const int mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (((op_ >> 6) & 3) == 3) {
        const int cc = (op_ >> 8) & 0xF;
        if (mode == 1) {  // DBcc
            const uint32_t base = pc;
            const int16_t disp = static_cast<int16_t>(fetch16());
            if (cond(cc)) { cyc_ += 12; return; }
            const uint16_t c = static_cast<uint16_t>(d[reg] - 1);
            d[reg] = (d[reg] & 0xFFFF0000u) | c;
            if (c == 0xFFFF) { cyc_ += 14; return; }
            pc = (base + disp) & 0xFFFFFF;
            cyc_ += 10;
            return;
        }
        Ea e = ea(mode, reg, 1, kDataAlt);  // Scc
        const bool t = cond(cc);
        ea_write(e, 1, t ? 0xFF : 0);
        cyc_ += mode == 0 ? (t ? 6 : 4) : 8;
        return;
    }
    const int sz = size_of((op_ >> 6) & 3);
    const uint32_t q = ((op_ >> 9) & 7) ? ((op_ >> 9) & 7) : 8;
    const bool sub = op_ & 0x100;
    cyc_ += mode <= 1 ? (sz == 4 ? 8 : 4) : (sz == 4 ? 12 : 8);
    Ea e = ea(mode, reg, sz, kAlt);
    if (mode == 1) {  // ADDQ/SUBQ em An: operacao de 32 bits sem flags
        a[reg] += sub ? -q : q;
        return;
    }
    ea_write(e, sz, alu(sub ? Alu::Sub : Alu::Add, ea_read(e, sz), q, sz));
}

void M68k::grp6() {
    const int cc = (op_ >> 8) & 0xF;
    const uint32_t base = pc;
    int32_t disp = static_cast<int8_t>(op_);
    const bool word = (op_ & 0xFF) == 0;
    if (word) disp = static_cast<int16_t>(fetch16());
    if (cc == 1) {  // BSR
        push32(pc);
        pc = (base + disp) & 0xFFFFFF;
        cyc_ += 18;
    } else if (cond(cc)) {
        pc = (base + disp) & 0xFFFFFF;
        cyc_ += 10;
    } else {
        cyc_ += word ? 12 : 8;
    }
}

void M68k::grp8() {
    const int opm = (op_ >> 6) & 7, mode = (op_ >> 3) & 7;
    if (opm == 3 || opm == 7) divide(opm == 7);
    else if (opm == 4 && mode < 2) bcd_op(true);
    else dyadic(Alu::Or);
}

void M68k::grp_addsub(bool add) {
    const int opm = (op_ >> 6) & 7, dn = (op_ >> 9) & 7, mode = (op_ >> 3) & 7, reg = op_ & 7;
    const Alu op = add ? Alu::Add : Alu::Sub;
    if (opm == 3 || opm == 7) {  // ADDA / SUBA
        const int sz = opm == 3 ? 2 : 4;
        cyc_ += sz == 4 ? 6 : 8;
        int32_t v = sext(ea_read(ea(mode, reg, sz, kAll), sz), sz);
        a[dn] += add ? v : -v;
    } else if (opm >= 4 && mode < 2) {  // ADDX / SUBX
        const int sz = size_of(opm & 3);
        cyc_ += sz == 4 ? 8 : 4;
        if (mode == 0) {
            d[dn] = (d[dn] & ~mask_of(sz)) | (alu(op, d[dn], d[reg], sz, true) & mask_of(sz));
        } else {
            Ea s = ea(4, reg, sz, kAll), t = ea(4, dn, sz, kAll);
            ea_write(t, sz, alu(op, ea_read(t, sz), ea_read(s, sz), sz, true));
        }
    } else {
        dyadic(op);
    }
}

void M68k::grpB() {
    const int opm = (op_ >> 6) & 7, dn = (op_ >> 9) & 7, mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (opm == 3 || opm == 7) {  // CMPA
        const int sz = opm == 3 ? 2 : 4;
        cyc_ += 6;
        alu(Alu::Cmp, a[dn], static_cast<uint32_t>(sext(ea_read(ea(mode, reg, sz, kAll), sz), sz)), 4);
        return;
    }
    const int sz = size_of(opm & 3);
    if (opm < 3) {  // CMP <ea>,Dn
        cyc_ += sz == 4 ? 6 : 4;
        alu(Alu::Cmp, d[dn], ea_read(ea(mode, reg, sz, kAll), sz), sz);
    } else if (mode == 1) {  // CMPM (Ay)+,(Ax)+
        cyc_ += 12;
        Ea s = ea(3, reg, sz, kAll), t = ea(3, dn, sz, kAll);
        alu(Alu::Cmp, ea_read(t, sz), ea_read(s, sz), sz);
    } else {  // EOR Dn,<ea>
        cyc_ += sz == 4 ? 12 : 8;
        Ea e = ea(mode, reg, sz, kDataAlt);
        ea_write(e, sz, alu(Alu::Eor, ea_read(e, sz), d[dn], sz));
    }
}

void M68k::grpC() {
    const int opm = (op_ >> 6) & 7, dn = (op_ >> 9) & 7, mode = (op_ >> 3) & 7, reg = op_ & 7;
    if (opm == 3 || opm == 7) { multiply(opm == 7); return; }
    if (opm == 4 && mode < 2) { bcd_op(false); return; }
    if (opm >= 5 && (mode < 2) && !(opm == 6 && mode == 0)) {  // EXG
        cyc_ += 6;
        const bool dd = opm == 5 && mode == 0, aa = opm == 5 && mode == 1;
        uint32_t& x = aa ? a[dn] : d[dn];
        uint32_t& y = dd ? d[reg] : a[reg];
        std::swap(x, y);
        return;
    }
    dyadic(Alu::And);
}

void M68k::grpE() {
    const int mode = (op_ >> 3) & 7, reg = op_ & 7;
    const bool left = op_ & 0x100;
    if (((op_ >> 6) & 3) == 3) {  // deslocamento de memoria (palavra, 1 posicao)
        if (op_ & 0x800) illegal();
        cyc_ += 8;
        Ea e = ea(mode, reg, 2, kMemAlt);
        ea_write(e, 2, shift((op_ >> 9) & 3, left, ea_read(e, 2), 1, 2));
        return;
    }
    const int sz = size_of((op_ >> 6) & 3), dn = (op_ >> 9) & 7;
    int cnt = (op_ & 0x20) ? static_cast<int>(d[dn] & 63) : (dn ? dn : 8);
    cyc_ += (sz == 4 ? 8 : 6) + 2 * cnt;
    d[reg] = (d[reg] & ~mask_of(sz)) | (shift((op_ >> 3) & 3, left, d[reg], cnt, sz) & mask_of(sz));
}

}  // namespace audio
