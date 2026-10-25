#pragma once
#include <string>

#include "saturnfs/sector_reader.hpp"

namespace saturnfs {

struct SaturnHeader {
    std::string maker, product_id, version, release_date, title;
};

// Lê o cabeçalho do setor 0. Lança se não começar com "SEGA SEGASATURN ".
SaturnHeader read_header(const SectorReader& source);

// Aceita apenas T-9527G V1.400; a mensagem de erro cita o ID encontrado.
void require_supported_disc(const SaturnHeader& header);

// Tamanho da faixa de dados (em setores) do T-9527G V1.400 original.
constexpr uint32_t kSupportedDataSectors = 204813;

// Recusa imagem cuja faixa de dados não tem `expected_sectors` setores (download/dump incompleto).
void require_complete_image(const SectorReader& source, uint32_t expected_sectors = kSupportedDataSectors);

}  // namespace saturnfs
