#include "sh2/onchip.hpp"

#include <algorithm>
#include <cstdint>

#include "saturn/lane.hpp"

namespace sh2 {
namespace {

// Offsets (a partir de 0xFFFFFE00).
constexpr uint32_t kTier = 0x10, kFtcsr = 0x11, kFrcHigh = 0x12, kFrcLow = 0x13, kOcrHigh = 0x14, kOcrLow = 0x15, kTcr = 0x16, kTocr = 0x17, kIcrHigh = 0x18, kIcrLow = 0x19;
constexpr uint32_t kIpraHigh = 0xE2;
constexpr uint32_t kBcr1MasterByte = 0x1E2;   // BCR1 (0xFFFFFFE0), byte com o bit 15 = MASTER
// DMAC (dois canais de 16 bytes: SAR, DAR, TCR, CHCR) e DMAOR.
constexpr uint32_t kDmacBegin = 0x180, kDmacEnd = 0x1B4, kDmacChannelStride = 0x10, kChcr = 0x0C, kVcrdma0 = 0x1A0, kVcrdmaStride = 8, kDmaor = 0x1B0;
constexpr uint32_t kChcrDe = 1, kChcrTe = 2, kChcrIe = 4, kChcrAutoRequest = 0x200;
constexpr uint32_t kDmaorDme = 1;
constexpr uint32_t kDivuBase = 0x100, kDivuEnd = 0x120;
constexpr uint32_t kDvsr = 0x100, kDvdnt = 0x104, kDvcr = 0x108, kVcrdiv = 0x10C, kDvdnth = 0x110, kDvdntl = 0x114;

constexpr uint32_t kDvcrOvf = 1, kDvcrOvfie = 2;
constexpr uint8_t kFtcsrFlags = 0x8E;   // ICF, OCFA, OCFB, OVF (zeram ao escrever 0)
constexpr uint8_t kFtcsrIcf = 0x80, kFtcsrOcfa = 0x08, kFtcsrOcfb = 0x04, kFtcsrOvf = 0x02;
constexpr uint8_t kTocrOcrs = 0x10;

uint32_t frt_divider(uint8_t tcr) {
    switch (tcr & 3) { case 0: return 8; case 1: return 32; case 2: return 128; default: return 0; }  // 3 = clock externo: não conta
}

bool within_ticks(uint16_t from, uint16_t target, uint64_t ticks) {  // `target` é atingido em (from, from + ticks]?
    return ticks >= 0x10000 || static_cast<uint16_t>(target - from - 1) < ticks;
}

}  // namespace

void OnChip::advance(uint64_t cycles) {
    const uint32_t divider = frt_divider(tcr_);
    if (divider == 0) return;
    frt_residue_ += cycles;
    const uint64_t ticks = frt_residue_ / divider;
    frt_residue_ %= divider;
    if (ticks == 0) return;

    if (within_ticks(frc_, ocra_, ticks)) ftcsr_ |= kFtcsrOcfa;
    if (within_ticks(frc_, ocrb_, ticks)) ftcsr_ |= kFtcsrOcfb;
    if (uint64_t(frc_) + ticks > 0xFFFF) ftcsr_ |= kFtcsrOvf;
    frc_ = static_cast<uint16_t>(frc_ + ticks);
}

void OnChip::save(saturn::StateWriter& w) const {
    w.raw(bytes_);
    for (uint32_t v : {dvsr_, dvdnth_, dvdntl_, dvcr_, vcrdiv_}) w.u32(v);
    for (uint16_t v : {frc_, ocra_, ocrb_, icr_}) w.u16(v);
    for (uint8_t v : {tier_, ftcsr_, tcr_, tocr_}) w.u8(v);
    w.u64(frt_residue_);
}

void OnChip::load(saturn::StateReader& r) {
    r.raw(bytes_);
    for (uint32_t* v : {&dvsr_, &dvdnth_, &dvdntl_, &dvcr_, &vcrdiv_}) *v = r.u32();
    for (uint16_t* v : {&frc_, &ocra_, &ocrb_, &icr_}) *v = r.u16();
    for (uint8_t* v : {&tier_, &ftcsr_, &tcr_, &tocr_}) *v = r.u8();
    frt_residue_ = r.u64();
}

void OnChip::reset() {
    std::fill(std::begin(bytes_), std::end(bytes_), uint8_t{0});
    dvsr_ = dvdnth_ = dvdntl_ = dvcr_ = vcrdiv_ = 0;
    frc_ = ocra_ = ocrb_ = icr_ = 0;
    tier_ = ftcsr_ = tcr_ = tocr_ = 0;
    frt_residue_ = 0;
}

void OnChip::signal_input_capture() {
    icr_ = frc_;
    ftcsr_ |= kFtcsrIcf;
}

// Divisão com sinal de 64/32 (a de 32/32 é o caso de dividendo estendido em sinal). Estouro: OVF=1, quociente saturado pelo
// sinal do resultado; resto = o calculado, ou a metade alta do dividendo na divisão por zero.
void OnChip::divide(int64_t dividend, int32_t divisor) {
    const bool by_zero = divisor == 0;
    const bool min_over_minus_one = dividend == INT64_MIN && divisor == -1;
    const int64_t quotient = (by_zero || min_over_minus_one) ? 0 : dividend / divisor;
    const int64_t remainder = (by_zero || min_over_minus_one) ? 0 : dividend % divisor;
    if (!by_zero && !min_over_minus_one && quotient <= INT32_MAX && quotient >= INT32_MIN) {
        dvdntl_ = static_cast<uint32_t>(quotient);
        dvdnth_ = static_cast<uint32_t>(remainder);
        return;
    }

    dvcr_ |= kDvcrOvf;
    dvdntl_ = ((dividend < 0) != (divisor < 0)) ? 0x80000000u : 0x7FFFFFFFu;
    dvdnth_ = by_zero ? static_cast<uint32_t>(dividend >> 32) : static_cast<uint32_t>(remainder);
    if ((dvcr_ & kDvcrOvfie) && irq) {
        const int level = ((bytes_[kIpraHigh] << 8 | bytes_[kIpraHigh + 1]) >> 12) & 0xF;
        if (level > 0) irq(level, static_cast<int>(vcrdiv_));
    }
}

uint32_t OnChip::reg32(uint32_t offset) const {
    return (uint32_t(bytes_[offset]) << 24) | (uint32_t(bytes_[offset + 1]) << 16) | (uint32_t(bytes_[offset + 2]) << 8) | bytes_[offset + 3];
}

void OnChip::set_reg32(uint32_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes_[offset + i] = (value >> (8 * (3 - i))) & 0xFF;
}

void OnChip::store_bytes(uint32_t offset, int bytes, uint32_t value) {
    for (int i = 0; i < bytes; ++i)
        if (offset + i < sizeof(bytes_)) bytes_[offset + i] = (value >> (8 * (bytes - 1 - i))) & 0xFF;
}

// CHCR: TE só zera por escrita de 0; o pedido automático com DE e DMAOR.DME ligados transfere tudo de uma vez.
void OnChip::write_dmac(uint32_t offset, int bytes, uint32_t value) {
    uint32_t old_te[2];
    for (int c = 0; c < 2; ++c) old_te[c] = reg32(kDmacBegin + c * kDmacChannelStride + kChcr) & kChcrTe;
    store_bytes(offset, bytes, value);
    for (int c = 0; c < 2; ++c) {
        const uint32_t chcr_offset = kDmacBegin + c * kDmacChannelStride + kChcr;
        set_reg32(chcr_offset, (reg32(chcr_offset) & ~kChcrTe) | (old_te[c] & reg32(chcr_offset)));
    }
    for (int c = 0; c < 2; ++c) run_dmac_channel(c);
}

void OnChip::run_dmac_channel(int channel) {
    if (!bus_) return;
    const uint32_t base = kDmacBegin + channel * kDmacChannelStride;
    const uint32_t chcr = reg32(base + kChcr);
    if (!(chcr & kChcrDe) || (chcr & kChcrTe) || !(chcr & kChcrAutoRequest) || !(reg32(kDmaor) & kDmaorDme)) return;

    static constexpr uint32_t kUnitBytes[4] = {1, 2, 4, 16};
    const uint32_t unit = kUnitBytes[(chcr >> 10) & 3];
    const auto step = [&](uint32_t mode) { return mode == 1 ? int32_t(unit) : mode == 2 ? -int32_t(unit) : 0; };
    const int32_t source_step = step((chcr >> 12) & 3), destination_step = step((chcr >> 14) & 3);
    uint32_t source = reg32(base), destination = reg32(base + 4), count = reg32(base + 8) & 0xFFFFFF;
    if (count == 0) count = 0x1000000;
    for (; count > 0; --count) {
        if (unit == 1) bus_->write8(destination, bus_->read8(source));
        else if (unit == 2) bus_->write16(destination, bus_->read16(source));
        else for (uint32_t i = 0; i < unit; i += 4) bus_->write32(destination + i, bus_->read32(source + i));
        source += source_step;
        destination += destination_step;
    }
    set_reg32(base, source);
    set_reg32(base + 4, destination);
    set_reg32(base + 8, 0);
    set_reg32(base + kChcr, chcr | kChcrTe);
    if ((chcr & kChcrIe) && irq) {
        const int level = ((bytes_[kIpraHigh] << 8 | bytes_[kIpraHigh + 1]) >> 8) & 0xF;
        if (level > 0) irq(level, static_cast<int>(reg32(kVcrdma0 + channel * kVcrdmaStride) & 0x7F));
    }
}

uint16_t& OnChip::selected_ocr() { return (tocr_ & kTocrOcrs) ? ocrb_ : ocra_; }

uint32_t OnChip::read_register(uint32_t offset) const {  // registrador de 32 bits da DIVU (offset alinhado)
    switch (offset) {
        case kDvsr: return dvsr_;
        case kDvdnt: return dvdntl_;
        case kDvcr: return dvcr_;
        case kVcrdiv: return vcrdiv_;
        case kDvdnth: return dvdnth_;
        case kDvdntl: return dvdntl_;
        default: return 0;
    }
}

void OnChip::write_register(uint32_t offset, uint32_t value) {
    switch (offset) {
        case kDvsr: dvsr_ = value; break;
        case kDvdnt: divide(static_cast<int32_t>(value), static_cast<int32_t>(dvsr_)); break;
        case kDvcr: dvcr_ = (value & kDvcrOvfie) | (dvcr_ & value & kDvcrOvf); break;  // OVF só zera
        case kVcrdiv: vcrdiv_ = value & 0x7F; break;
        case kDvdnth: dvdnth_ = value; break;
        case kDvdntl: dvdntl_ = value; divide(static_cast<int64_t>((uint64_t(dvdnth_) << 32) | dvdntl_), static_cast<int32_t>(dvsr_)); break;
        default: break;
    }
}

uint32_t OnChip::read(uint32_t offset, int bytes) {
    if (offset >= kDivuBase && offset < kDivuEnd) {
        const uint32_t word = read_register(offset & ~3u);
        return saturn::extract_lane(word, offset, bytes);
    }
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        const uint32_t at = offset + i;
        uint8_t byte;
        switch (at) {
            case kTier: byte = tier_; break;
            case kFtcsr: byte = ftcsr_; break;
            case kFrcHigh: byte = frc_ >> 8; break;
            case kFrcLow: byte = frc_ & 0xFF; break;
            case kOcrHigh: byte = selected_ocr() >> 8; break;
            case kOcrLow: byte = selected_ocr() & 0xFF; break;
            case kTcr: byte = tcr_; break;
            case kTocr: byte = tocr_; break;
            case kIcrHigh: byte = icr_ >> 8; break;
            case kIcrLow: byte = icr_ & 0xFF; break;
            case kBcr1MasterByte: byte = (bytes_[at] & 0x7F) | (slave_ ? 0x80 : 0); break;
            default: byte = at < sizeof(bytes_) ? bytes_[at] : 0;
        }
        value = (value << 8) | byte;
    }
    return value;
}

void OnChip::write(uint32_t offset, int bytes, uint32_t value) {
    if (offset >= kDivuBase && offset < kDivuEnd) {
        write_register(offset & ~3u, saturn::insert_lane(read_register(offset & ~3u), offset, bytes, value));
        return;
    }
    if (offset >= kDmacBegin && offset < kDmacEnd) {
        write_dmac(offset, bytes, value);
        return;
    }
    for (int i = 0; i < bytes; ++i) {
        const uint32_t at = offset + i;
        const uint8_t byte = (value >> (8 * (bytes - 1 - i))) & 0xFF;
        switch (at) {
            case kTier: tier_ = byte; break;
            case kFtcsr: ftcsr_ = (ftcsr_ & byte & kFtcsrFlags) | (byte & 0x01); break;  // flags zeram ao escrever 0
            case kFrcHigh: frc_ = (frc_ & 0x00FF) | (byte << 8); break;
            case kFrcLow: frc_ = (frc_ & 0xFF00) | byte; break;
            case kOcrHigh: { uint16_t& ocr = selected_ocr(); ocr = (ocr & 0x00FF) | (byte << 8); break; }
            case kOcrLow: { uint16_t& ocr = selected_ocr(); ocr = (ocr & 0xFF00) | byte; break; }
            case kTcr: tcr_ = byte; break;
            case kTocr: tocr_ = byte; break;
            default: if (at < sizeof(bytes_)) bytes_[at] = byte;
        }
    }
}

}  // namespace sh2
