#include "saturn/scu.hpp"

#include <algorithm>
#include <array>

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
constexpr uint32_t kDmaEnableBit = 0x100, kDmaGoBit = 0x1, kDmaStartSoftware = 0x7;
constexpr uint32_t kTimer1Enable = 0x1, kTimer1WhenTimer0 = 0x100;
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

uint32_t Scu::read(uint32_t offset, int bytes) { return extract_lane(read_register(offset & ~3u), offset, bytes); }

void Scu::write(uint32_t offset, int bytes, uint32_t value) {
    write_register(offset & ~3u, insert_lane(read_register(offset & ~3u), offset, bytes, value));
}

void Scu::run_dma(int level) {
    const uint32_t base = level * kDmaStride;
    uint32_t read_address = regs_[(base + kDmaRead) / 4], write_address = regs_[(base + kDmaWrite) / 4];
    const uint32_t add = regs_[(base + kDmaAdd) / 4];
    const uint32_t read_step = (add & 0x100) ? 4 : 0, write_step = kWriteAdd[add & 7];
    uint32_t count = regs_[(base + kDmaCount) / 4] & (level == 0 ? 0xFFFFF : 0xFFF);
    if (count == 0) count = level == 0 ? 0x100000 : 0x1000;

    for (uint32_t done = 0; done < count; done += 4) {
        bus_.write32(write_address, bus_.read32(read_address));
        read_address += read_step;
        write_address += write_step;
    }
    regs_[(base + kDmaEnable) / 4] &= ~kDmaGoBit;
    raise(level == 0 ? Interrupt::Dma0End : level == 1 ? Interrupt::Dma1End : Interrupt::Dma2End);
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
