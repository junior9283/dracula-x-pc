#pragma once
#include <string>

#include "saturnfs/image.hpp"

namespace saturnfs {

struct SaturnHeader {
    std::string maker, product_id, version, release_date, title;
};

// Lê o cabeçalho do setor 0. Lança se não começar com "SEGA SEGASATURN ".
SaturnHeader read_header(const DiscImage& disc);

// Aceita apenas T-9527G V1.400; a mensagem de erro cita o ID encontrado.
void require_supported_disc(const SaturnHeader& header);

}  // namespace saturnfs
