// Arquivos dos save states: <pasta de dados>/states/slotN.dxs, gravados com troca atômica.
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "frontend/fileio.hpp"

namespace frontend {

std::filesystem::path state_path(const std::filesystem::path& data_dir, int slot);   // slot 0..7 -> slot1.dxs..slot8.dxs
std::string format_time(std::time_t when);                                                    // "07/11/2026 14:30"
// Data/hora da última gravação do arquivo, ou "vazio" se ele não existe.
std::string slot_description(const std::filesystem::path& file);

}  // namespace frontend
