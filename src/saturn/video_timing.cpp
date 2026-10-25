#include "saturn/video_timing.hpp"

#include <algorithm>

namespace saturn {
namespace {
constexpr uint32_t kTvstatOffset = 0x04;
constexpr uint16_t kTvstatVBlank = 1u << 3, kTvstatHBlank = 1u << 2, kTvstatOdd = 1u << 1;
}  // namespace

void VideoTiming::advance(uint64_t cycles) {
    while (cycles > 0) {
        const uint32_t next_mark = hblank_ ? kCyclesPerLine : kHBlankStart;
        const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(cycles, next_mark - cycle_));
        cycle_ += step;
        cycles -= step;

        if (!hblank_ && cycle_ == kHBlankStart) {
            hblank_ = true;
            scu_.raise(Scu::Interrupt::HBlankIn);
        } else if (cycle_ == kCyclesPerLine) {
            cycle_ = 0;
            hblank_ = false;
            if (++line_ == kLines) {
                line_ = 0;
                ++frame_;
                vblank_ = false;
                scu_.raise(Scu::Interrupt::VBlankOut);
            } else if (line_ == kVBlankLine) {
                vblank_ = true;
                scu_.raise(Scu::Interrupt::VBlankIn);
            }
        }
    }
}

uint16_t VideoTiming::tvstat() const {
    return (vblank_ ? kTvstatVBlank : 0) | (hblank_ ? kTvstatHBlank : 0) | ((frame_ & 1) ? kTvstatOdd : 0);
}

uint32_t VideoTiming::read(uint32_t offset, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        const uint32_t at = offset + i;
        uint8_t byte = at < regs_.size() ? regs_[at] : 0;
        if (at == kTvstatOffset) byte = tvstat() >> 8;
        if (at == kTvstatOffset + 1) byte = tvstat() & 0xFF;
        value = (value << 8) | byte;
    }
    return value;
}

void VideoTiming::write(uint32_t offset, int bytes, uint32_t value) {
    for (int i = 0; i < bytes; ++i) {
        const uint32_t at = offset + i;
        if (at < regs_.size() && at != kTvstatOffset && at != kTvstatOffset + 1) regs_[at] = (value >> (8 * (bytes - 1 - i))) & 0xFF;
    }
}

}  // namespace saturn
