#include "saturnfs/overlay.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "saturnfs/strutil.hpp"

namespace saturnfs {
namespace {

constexpr uint32_t kRamLow = 0x06000000;
constexpr uint32_t kRamHigh = 0x06100000;

struct BaseGroup {
    uint32_t base;
    std::array<std::string_view, 16> exact;     // nomes exatos (minúsculos)
    std::array<std::string_view, 6> prefixes;   // prefixos (minúsculos)
};

// Agrupamento observado em docs/analysis/01-overlays.md (método de votação + destinos lidos do código).
constexpr std::array<BaseGroup, 5> kGroups = {{
    {0x060A5000, {"logo.prg", "open.prg", "staff.prg", "title.prg", "alucard.prg", "maria.prg", "richter.prg", "richter2.prg"}, {}},
    {0x06066000, {"game.prg", "movie.prg"}, {}},
    {0x060CF000, {}, {"t_"}},
    {0x060D6000, {"sub.prg"}, {}},
    {0x060DC000, {"ending.prg", "library.prg", "warp.prg", "rwarp.prg"}, {"stage_", "rstage", "boss_"}},
}};

uint32_t be32(std::span<const uint8_t> bytes, size_t offset) {
    return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) | (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

}  // namespace

std::optional<uint32_t> known_overlay_base(std::string_view name) {
    const std::string lowered = to_lower(name);
    for (const auto& group : kGroups) {
        if (std::ranges::find(group.exact, lowered) != group.exact.end()) return group.base;
        for (auto prefix : group.prefixes)
            if (!prefix.empty() && lowered.starts_with(prefix)) return group.base;
    }
    return std::nullopt;
}

OverlayInfo parse_overlay(std::string_view name, std::span<const uint8_t> bytes) {
    auto base = known_overlay_base(name);
    if (!base) throw std::runtime_error("overlay sem endereço de carga conhecido: " + std::string(name));
    if (bytes.size() < 4) throw std::runtime_error("overlay menor que uma palavra: " + std::string(name));

    OverlayInfo info{std::string(name), *base, static_cast<uint32_t>(bytes.size()), {}};
    for (size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
        uint32_t word = be32(bytes, offset);
        if (word < kRamLow || word >= kRamHigh || word < info.load_address || word >= info.load_address + info.size) break;
        info.header_table.push_back(word);
    }
    return info;
}

std::vector<std::string> find_overlay_conflicts(std::span<const OverlayInfo> overlays) {
    std::vector<std::string> conflicts;
    for (size_t i = 0; i < overlays.size(); ++i)
        for (size_t j = i + 1; j < overlays.size(); ++j) {
            const auto& a = overlays[i];
            const auto& b = overlays[j];
            bool overlap = a.load_address < b.load_address + b.size && b.load_address < a.load_address + a.size;
            if (overlap && a.load_address != b.load_address) conflicts.push_back(a.name + " x " + b.name);
        }
    return conflicts;
}

}  // namespace saturnfs
