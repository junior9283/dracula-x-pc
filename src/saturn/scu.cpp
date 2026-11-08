#include "saturn/scu.hpp"

#include <algorithm>
#include <array>

#include "saturn/b_bus.hpp"
#include "saturn/lane.hpp"

namespace saturn {
namespace {
struct Source { int level, vector; };
// Em ordem de Scu::Interrupt (índice = bit em IMS/IST).
constexpr std::array<Source, 14> kSources = {{
    {0xF, 0x40}, {0xE, 0x41}, {0xD, 0x42}, {0xC, 0x43}, {0xB, 0x44}, {0xA, 0x45}, {9, 0x46},
    {8, 0x47},   {8, 0x48},   {6, 0x49},   {6, 0x4A},   {5, 0x4B},   {3, 0x4C},   {2, 0x4D}}};
constexpr uint32_t kInterruptBits = (1u << kSources.size()) - 1;

constexpr uint32_t kT0c = 0x90, kT1md = 0x98, kIms = 0xA0, kIst = 0xA4;
constexpr uint32_t kDmaStride = 0x20, kDmaRead = 0x00, kDmaWrite = 0x04, kDmaCount = 0x08, kDmaAdd = 0x0C, kDmaEnable = 0x10, kDmaMode = 0x14;
constexpr uint32_t kDmaEnableBit = 0x100, kDmaGoBit = 0x1, kDmaStartSoftware = 0x7, kDmaIndirect = 0x01000000;
constexpr uint32_t kTimer1Enable = 0x1, kTimer1WhenTimer0 = 0x100;
constexpr bool is_dsp_register(uint32_t offset) { return offset >= 0x80 && offset < 0x90; }
constexpr std::array<uint32_t, 8> kWriteAdd = {0, 2, 4, 8, 16, 32, 64, 128};

}  // namespace

uint32_t Scu::read_register(uint32_t offset) const {
    if (offset == kIms) return ims_;
    if (offset == kIst) return ist_;
    return offset < sizeof(regs_) ? regs_[offset / 4] : 0;
}

void Scu::write_register(uint32_t offset, uint32_t value) {
    if (offset == kIms) { ims_ = value; update_pending(); return; }
    if (offset == kIst) { ist_ &= value; return; }  // escrever 0 limpa o bit
    if (offset >= sizeof(regs_)) return;
    regs_[offset / 4] = value;

    // Disparo do DMA: habilita + GO com fator de início por software (modo direto).
    for (int level = 0; level < 3; ++level) {
        if (offset != level * kDmaStride + kDmaEnable) continue;
        const uint32_t mode = regs_[(level * kDmaStride + kDmaMode) / 4];
        if ((value & kDmaEnableBit) && (value & kDmaGoBit) && (mode & 7) == kDmaStartSoftware) run_dma(level);
    }
}

uint32_t Scu::read(uint32_t offset, int bytes) {
    if (is_dsp_register(offset)) return extract_lane(dsp_.read(offset & ~3u), offset, bytes);
    return extract_lane(read_register(offset & ~3u), offset, bytes);
}

void Scu::write(uint32_t offset, int bytes, uint32_t value) {
    if (is_dsp_register(offset)) { dsp_.write(offset & ~3u, insert_lane(0, offset, bytes, value)); return; }
    write_register(offset & ~3u, insert_lane(read_register(offset & ~3u), offset, bytes, value));
}

void Scu::run_dma(int level) {
    const uint32_t base = level * kDmaStride;
    const uint32_t add = regs_[(base + kDmaAdd) / 4];
    const uint32_t read_step = (add & 0x100) ? 4 : 0, write_step = kWriteAdd[add & 7];
    const uint32_t count_mask = level == 0 ? 0xFFFFF : 0xFFF;
    const auto copy = [&](uint32_t read_address, uint32_t write_address, uint32_t count) {
        if (count == 0) count = level == 0 ? 0x100000 : 0x1000;
        const bool b_bus = is_b_bus(write_address & 0x1FFF'FFFF);   // sem os bits de cache
        for (uint32_t done = 0; done < count; done += 4) {
            const uint32_t value = bus_.read32(read_address);
            if (b_bus) write_address = write_b_bus_long(bus_, write_address, value, write_step);
            else bus_.write32(write_address, value);
            read_address += read_step;
            write_address += write_step;
        }
    };

    if (regs_[(base + kDmaMode) / 4] & kDmaIndirect) {
        // Modo indireto: D0W aponta para uma tabela de entradas (contagem, destino, origem); o bit 31 da origem marca a última.
        for (uint32_t table = regs_[(base + kDmaWrite) / 4];; table += 12) {
            const uint32_t count = bus_.read32(table) & count_mask, destination = bus_.read32(table + 4), source = bus_.read32(table + 8);
            copy(source & 0x7FFFFFFF, destination, count);
            if (source & 0x80000000u) break;
        }
    } else {
        copy(regs_[(base + kDmaRead) / 4], regs_[(base + kDmaWrite) / 4], regs_[(base + kDmaCount) / 4] & count_mask);
    }
    regs_[(base + kDmaEnable) / 4] &= ~kDmaGoBit;
    raise(level == 0 ? Interrupt::Dma0End : level == 1 ? Interrupt::Dma1End : Interrupt::Dma2End);
}

void Scu::reset() {
    ims_ = 0xBFFF;
    ist_ = 0;
    for (int level = 0; level < 3; ++level) regs_[(level * kDmaStride + kDmaEnable) / 4] = 0;
    dsp_.reset();
}

void Scu::save(StateWriter& w) const {
    for (uint32_t v : regs_) w.u32(v);
    w.u32(ims_);
    w.u32(ist_);
    w.u32(line_counter_);
    dsp_.save(w);
}

void Scu::load(StateReader& r) {
    for (uint32_t& v : regs_) v = r.u32();
    ims_ = r.u32();
    ist_ = r.u32();
    line_counter_ = r.u32();
    dsp_.load(r);
}

void Scu::raise(Interrupt source) {
    ist_ |= 1u << static_cast<int>(source);
    if (source == Interrupt::VBlankOut) line_counter_ = 0;
    if (source == Interrupt::HBlankIn) {
        ++line_counter_;
        const bool timer0_hit = line_counter_ == regs_[kT0c / 4];
        if (timer0_hit) ist_ |= 1u << static_cast<int>(Interrupt::Timer0);
        const uint32_t t1 = regs_[kT1md / 4];
        if ((t1 & kTimer1Enable) && (!(t1 & kTimer1WhenTimer0) || timer0_hit)) ist_ |= 1u << static_cast<int>(Interrupt::Timer1);
    }
    update_pending();
}

void Scu::acknowledge(int vector) {
    for (size_t i = 0; i < kSources.size(); ++i)
        if (kSources[i].vector == vector) ist_ &= ~(1u << i);
    update_pending();
}

void Scu::update_pending() {
    const uint32_t pending = ist_ & ~ims_ & kInterruptBits;
    if (!pending || !deliver) return;
    int best = -1;
    for (size_t i = 0; i < kSources.size(); ++i)
        if ((pending >> i & 1) && (best < 0 || kSources[i].level > kSources[best].level)) best = static_cast<int>(i);
    deliver(kSources[best].level, kSources[best].vector);
}

}  // namespace saturn
