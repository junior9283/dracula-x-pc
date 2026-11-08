#include "frontend/states.hpp"

#include <chrono>

namespace frontend {

std::filesystem::path state_path(const std::filesystem::path& data_dir, int slot) {
    return data_dir / "states" / ("slot" + std::to_string(slot + 1) + ".dxs");
}

std::string format_time(std::time_t when) {
    const std::tm* local = std::localtime(&when);
    if (!local) return "?";
    char text[32];
    return std::strftime(text, sizeof text, "%d/%m/%Y %H:%M", local) ? text : "?";
}

std::string slot_description(const std::filesystem::path& file) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) return "vazio";
    // Converte o relógio dos arquivos para o do sistema (clock_cast ainda não existe em todos os compiladores).
    const auto system_time = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                                    written - std::filesystem::file_time_type::clock::now());
    return format_time(std::chrono::system_clock::to_time_t(system_time));
}

}  // namespace frontend
