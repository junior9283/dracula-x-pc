#include "saturn/video_timing.hpp"

#include <algorithm>

namespace saturn {
namespace {
constexpr uint32_t kTvstatOffset = 0x04;
constexpr uint16_t kTvstatVBlank = 1u << 3, kTvstatHBlank = 1u << 2, kTvstatOdd = 1u << 1;
}  // namespace

void VideoTiming::advance(uint64_t cycles) {
    while (cycles > 0) {
        // O modo pode mudar no meio da linha: uma marca que já ficou para trás vale como alcançada.
        const uint32_t line_end = cycles_per_line(), hblank_at = hblank_start();
        const uint32_t next_mark = std::max(cycle_, hblank_ ? line_end : hblank_at);
        const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(cycles, next_mark - cycle_));
        cycle_ += step;
        cycles -= step;

        if (!hblank_ && cycle_ >= hblank_at) {
            hblank_ = true;
            scu_.raise(Scu::Interrupt::HBlankIn);
        } else if (hblank_ && cycle_ >= line_end) {
            cycle_ = 0;
            hblank_ = false;
            if (++line_ == kLines) {
                line_ = 0;
                ++frame_;
                vblank_ = false;
                scu_.raise(Scu::Interrupt::VBlankOut);
            } else if (line_ == visible_lines()) {
                vblank_ = true;
                scu_.raise(Scu::Interrupt::VBlankIn);
            }
        }
    }
}

void VideoTiming::save(StateWriter& w) const {
    w.raw(regs_);
    w.u32(line_);
    w.u32(cycle_);
    w.boolean(hblank_);
    w.boolean(vblank_);
    w.u64(frame_);
}

void VideoTiming::load(StateReader& r) {
    r.raw(regs_);
    line_ = r.u32();
    cycle_ = r.u32();
    hblank_ = r.boolean();
    vblank_ = r.boolean();
    frame_ = r.u64();
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
