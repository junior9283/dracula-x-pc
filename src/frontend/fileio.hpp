// Arquivos do host: leitura inteira e gravação com troca atômica (.tmp + rename), para saves, estados, configuração e ferramentas.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace frontend {

// Lança std::runtime_error se não conseguir abrir ou ler.
std::vector<uint8_t> read_file_bytes(const std::filesystem::path& file);
// Vazio se o arquivo não existir ou não puder ser lido.
std::optional<std::vector<uint8_t>> try_read_file_bytes(const std::filesystem::path& file);
// Cria a pasta se preciso; um arquivo antigo nunca fica pela metade. Lança std::runtime_error.
void write_file_atomic(const std::filesystem::path& file, std::span<const uint8_t> bytes);

}  // namespace frontend
