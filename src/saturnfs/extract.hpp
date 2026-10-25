#pragma once
#include <filesystem>

#include "saturnfs/sector_reader.hpp"

namespace saturnfs {

// Extrai todos os arquivos do ISO9660 para `out` e grava `manifest.txt` (caminho lba tamanho sha256).
// Lança se `out` já tem conteúdo e !force. Retorna o número de arquivos extraídos.
size_t extract_all(const SectorReader& source, const std::filesystem::path& out, bool force);

}  // namespace saturnfs
