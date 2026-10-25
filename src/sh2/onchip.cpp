#include "sh2/onchip.hpp"

#include <cstdint>

#include "saturn/lane.hpp"

namespace sh2 {
namespace {

// Offsets (a partir de 0xFFFFFE00).
constexpr uint32_t kTier = 0x10, kFtcsr = 0x11, kFrcHigh = 0x12, kFrcLow = 0x13, kOcrHigh = 0x14, kOcrLow = 0x15, kTcr = 0x16, kTocr = 0x17;
constexpr uint32_t kIpraHigh = 0xE2;
constexpr uint32_t kDivuBase = 0x100, kDivuEnd = 0x120;
constexpr uint32_t kDvsr = 0x100, kDvdnt = 0x104, kDvcr = 0x108, kVcrdiv = 0x10C, kDvdnth = 0x110, kDvdntl = 0x114;

constexpr uint32_t kDvcrOvf = 1, kDvcrOvfie = 2;
constexpr uint8_t kFtcsrFlags = 0x8E;   // ICF, OCFA, OCFB, OVF (zeram ao escrever 0)
constexpr uint8_t kFtcsrOcfa = 0x08, kFtcsrOcfb = 0x04, kFtcsrOvf = 0x02;
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
