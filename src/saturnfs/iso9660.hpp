#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "saturnfs/sector_reader.hpp"

namespace saturnfs {

struct FileEntry {
    std::string path;  // relativo, separador '/', sem ";1"
    uint32_t lba;
    uint32_t size;
};

// Entrada de um diretório ISO9660 (nome sem ";1"; "." e ".." aparecem com esses nomes).
struct DirEntry {
    std::string name;
    uint32_t lba;
    uint32_t size;
    bool is_directory;
};

// Registro do diretório raiz (a partir do descritor primário no setor 16).
DirEntry root_directory(const SectorReader& source);
// Entradas de um diretório, na ordem do disco, incluindo "." e "..". Lança em registro inválido.
std::vector<DirEntry> list_directory(const SectorReader& source, uint32_t lba, uint32_t size);

// Lista os arquivos do ISO9660 (recursivo). Entradas com nome "." ou ".." são ignoradas.
std::vector<FileEntry> list_files(const SectorReader& source);

// Falso para arquivos fantasmas cujo extent cai fora da faixa de dados (ex.: CD3, sobre a faixa de áudio).
bool is_in_data_track(const SectorReader& source, const FileEntry& entry);

std::vector<uint8_t> read_file(const SectorReader& source, const FileEntry& entry);

}  // namespace saturnfs
