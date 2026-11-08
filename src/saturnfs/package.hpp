// Pacote local do port ("gere com a sua ISO"): os setores de usuário da faixa de dados copiados do disco do próprio usuário, para
// que o jogo rode sem a ISO. Formato "DXPCPAK1": cabeçalho de 64 bytes (assinatura, versão, nº de setores, SHA-256 dos setores)
// seguido dos setores de 2048 bytes em ordem de LBA. Nunca é distribuído: é gerado na máquina de quem tem o disco.
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "saturnfs/sector_reader.hpp"

namespace saturnfs {

// Progresso da geração: setores copiados e total.
using PackageProgress = std::function<void(uint32_t done, uint32_t total)>;

// Grava o pacote (num arquivo temporário trocado no fim) e devolve o SHA-256 dos setores, em hexadecimal. Com `expected_digest`,
// lança e não deixa arquivo algum se o conteúdo não tiver esse resumo.
std::string write_package(const SectorReader& source, const std::filesystem::path& path, const PackageProgress& progress = {},
                          std::string_view expected_digest = {});

class PackageReader : public SectorReader {
public:
    // Lança se o arquivo não for um pacote ou estiver incompleto.
    static PackageReader open(const std::filesystem::path& path);

    std::array<uint8_t, kUserSectorSize> read_user_sector(uint32_t lba) const override;
    uint32_t sector_count() const override { return sectors_; }
    const std::string& digest() const { return digest_; }
    // Recalcula o SHA-256 de todos os setores e compara com o do cabeçalho (lento: lê o pacote inteiro).
    bool verify() const;

private:
    static constexpr size_t kReadBuffer = 256 * 1024;
    std::unique_ptr<char[]> buffer_;   // buffer do ifstream (precisa viver tanto quanto ele)
    mutable std::ifstream file_;
    mutable uint32_t next_lba_ = ~0u;  // posição do arquivo, em LBA; evita seek em leitura sequencial
    uint32_t sectors_ = 0;
    std::string digest_;
};

}  // namespace saturnfs
