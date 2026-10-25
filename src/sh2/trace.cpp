#include "sh2/trace.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace sh2 {
namespace {

constexpr char kMagic[4] = {'D', 'X', 'T', 'R'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;

void put32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof v); }  // little-endian (hosts alvo)
bool get32(std::ifstream& in, uint32_t& v) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof v)); }

}  // namespace

ExecutionRecorder::ExecutionRecorder() : blocks_(kBlocks) {}

void ExecutionRecorder::record(uint32_t pc) {
    pc &= kPhysicalMask;
    auto& block = blocks_[pc >> kBlockShift];
    if (!block) block = std::make_unique<Block>();  // zerado
    const uint32_t word = (pc & ((1u << kBlockShift) - 1)) >> 1;
    uint64_t& bits = (*block)[word >> 6];
    const uint64_t mask = uint64_t(1) << (word & 63);
    if (!(bits & mask)) { bits |= mask; ++count_; }
}

bool ExecutionRecorder::executed(uint32_t pc) const {
    pc &= kPhysicalMask;
    const auto& block = blocks_[pc >> kBlockShift];
    if (!block) return false;
    const uint32_t word = (pc & ((1u << kBlockShift) - 1)) >> 1;
    return ((*block)[word >> 6] >> (word & 63)) & 1;
}

void ExecutionRecorder::save(const std::filesystem::path& file) const {
    // Faixas de instruções consecutivas: (endereço inicial, quantidade).
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (uint32_t b = 0; b < kBlocks; ++b) {
        if (!blocks_[b]) continue;
        const uint32_t base = b << kBlockShift;
        for (uint32_t w = 0; w < blocks_[b]->size(); ++w) {
            uint64_t bits = (*blocks_[b])[w];
            while (bits) {
                const uint32_t bit = std::countr_zero(bits);
                bits &= bits - 1;
                const uint32_t address = base + ((w * 64 + bit) << 1);
                if (!runs.empty() && runs.back().first + runs.back().second * 2 == address) ++runs.back().second;
                else runs.emplace_back(address, 1);
            }
        }
    }
    std::ofstream out(file, std::ios::binary);
    out.write(kMagic, sizeof kMagic);
    put32(out, kVersion);
    put32(out, static_cast<uint32_t>(runs.size()));
    for (const auto& [start, length] : runs) { put32(out, start); put32(out, length); }
    if (!out) throw std::runtime_error("falha ao gravar o traço: " + file.string());
}

ExecutionRecorder ExecutionRecorder::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    char magic[4] = {};
    uint32_t version = 0, runs = 0;
    if (!in.read(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || !get32(in, version) || version != kVersion ||
        !get32(in, runs))
        throw std::runtime_error("arquivo de traço inválido: " + file.string());

    ExecutionRecorder recorder;
    for (uint32_t i = 0; i < runs; ++i) {
        uint32_t start = 0, length = 0;
        if (!get32(in, start) || !get32(in, length)) throw std::runtime_error("arquivo de traço truncado: " + file.string());
        for (uint32_t k = 0; k < length; ++k) recorder.record(start + k * 2);
    }
    return recorder;
}

}  // namespace sh2
