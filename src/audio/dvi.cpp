#include "audio/dvi.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace audio {
namespace {

constexpr size_t kHeaderBytes = 0x800;
constexpr int kMaxStepIndex = 88;
constexpr size_t kGroupBytes = 8;       // 4 bytes de L + 4 bytes de R
constexpr size_t kBytesPerChannelGroup = 4;

constexpr std::array<int, 89> kStepTable = {
    7,    8,    9,    10,   11,   12,   13,   14,   16,   17,   19,   21,   23,    25,    28,    31,    34,
    37,   41,   45,   50,   55,   60,   66,   73,   80,   88,   97,   107,  118,   130,   143,   157,   173,
    190,  209,  230,  253,  279,  307,  337,  371,  408,  449,  494,  544,  598,   658,   724,   796,   876,
    963,  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,  3327,  3660,  4026,  4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};
constexpr std::array<int, 8> kIndexAdjust = {-1, -1, -1, -1, 2, 4, 6, 8};

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3];
}

int16_t decode_nibble(DviChannelState& s, unsigned nibble) {
    const int step = kStepTable[s.index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    s.predictor = static_cast<int16_t>(std::clamp(s.predictor + diff, -32768, 32767));
    s.index = std::clamp(s.index + kIndexAdjust[nibble & 7], 0, kMaxStepIndex);
    return s.predictor;
}

}  // namespace

DviHeader parse_dvi_header(std::span<const uint8_t> file) {
    if (file.size() < kHeaderBytes) throw std::runtime_error("arquivo DVI menor que o cabeçalho de 0x800 bytes");
    if (file[0] != 'D' || file[1] != 'V' || file[2] != 'I' || file[3] != '.') throw std::runtime_error("magic DVI. ausente");

    DviHeader h{};
    h.header_size = be32(file, 4);
    h.body_size = be32(file, 8);
    h.loop_start_frames = be32(file, 0xC);
    if (uint64_t(h.header_size) + h.body_size != file.size())
        throw std::runtime_error("tamanho do corpo inconsistente com o tamanho do arquivo");

    h.channel[0] = {static_cast<int16_t>(be32(file, 0x10) >> 16), static_cast<int>(be32(file, 0x14))};
    h.channel[1] = {static_cast<int16_t>(be32(file, 0x20) >> 16), static_cast<int>(be32(file, 0x24))};
    for (const auto& c : h.channel)
        if (c.index < 0 || c.index > kMaxStepIndex) throw std::runtime_error("índice de passo fora de 0..88: " + std::to_string(c.index));
    return h;
}

std::vector<int16_t> decode_dvi(std::span<const uint8_t> body, const DviHeader& header) {
    const size_t groups = body.size() / kGroupBytes;
    std::vector<int16_t> pcm;
    pcm.reserve(groups * kBytesPerChannelGroup * 2 * 2);

    DviChannelState left = header.channel[0];
    DviChannelState right = header.channel[1];
    for (size_t g = 0; g < groups; ++g) {
        const uint8_t* l = body.data() + g * kGroupBytes;
        const uint8_t* r = l + kBytesPerChannelGroup;
        for (size_t i = 0; i < kBytesPerChannelGroup; ++i)
            for (unsigned shift : {4u, 0u}) {
                pcm.push_back(decode_nibble(left, (l[i] >> shift) & 0xF));
                pcm.push_back(decode_nibble(right, (r[i] >> shift) & 0xF));
            }
    }
    return pcm;
}

}  // namespace audio
