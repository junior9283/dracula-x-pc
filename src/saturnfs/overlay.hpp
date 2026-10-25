#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace saturnfs {

struct OverlayInfo {
    std::string name;
    uint32_t load_address;
    uint32_t size;
    std::vector<uint32_t> header_table;  // ponteiros do início do arquivo, todos dentro de [load_address, load_address+size)
};

// Endereço de carga (Work RAM-H) dos overlays .PRG, extraído do código do jogo (docs/analysis/01-overlays.md).
// Vazio para nomes desconhecidos e para WEAPON0/1.PRG (dados, não código).
std::optional<uint32_t> known_overlay_base(std::string_view name);

// Lança std::runtime_error (citando o nome) se o nome não tem base conhecida ou o arquivo tem menos de 4 bytes.
OverlayInfo parse_overlay(std::string_view name, std::span<const uint8_t> bytes);

// Pares "A x B" cujas regiões se sobrepõem parcialmente com bases diferentes (mesma base = alternativas do mesmo slot).
std::vector<std::string> find_overlay_conflicts(std::span<const OverlayInfo> overlays);

}  // namespace saturnfs
