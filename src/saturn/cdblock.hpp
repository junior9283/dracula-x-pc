#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "saturn/bus.hpp"
#include "saturn/state.hpp"
#include "saturnfs/iso9660.hpp"
#include "saturnfs/sector_reader.hpp"

namespace saturn {

// Bloco de CD do Saturn (registradores em 0x25890000 e porta de dados em 0x25818000), servido por uma `SectorReader`.
// Os comandos executam instantaneamente: o software escreve CR1–CR4 (CR4 dispara) e lê a resposta nos mesmos registradores,
// com o fim sinalizado em HIRQ. Formatos de comando/resposta: INFERÊNCIA a partir da documentação do Saturn.
class CdBlock : public IoDevice {
public:
    static constexpr uint32_t kRegisterBase = 0x05890000;   // 0x25890000 sem o bit de cache-through
    static constexpr uint32_t kDataPortBase = 0x05818000;

    explicit CdBlock(const saturnfs::SectorReader& disc);
    ~CdBlock() override;

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;
    IoDevice& data_port();

    // Observador de depuração: comando recebido (CR1–CR4) e a resposta devolvida.
    std::function<void(const std::array<uint16_t, 4>& command, const std::array<uint16_t, 4>& reply)> on_command;

    // Tempo do drive: a cada 1/75 s (um quadro de subcódigo) liga HIRQ.SCDQ.
    static constexpr uint32_t kCyclesPerSubcodeFrame = 381818;   // 28,636 MHz / 75
    void advance(uint64_t cycles);
    // Estado entregue pelo BIOS ao jogo: a troca de disco (DCHG) já foi tratada.
    void acknowledge_disc_change() { hirq_ &= static_cast<uint16_t>(~0x0020u); }
    // Autenticação do disco (comando 0xE0; o boot por HLE a faz como o BIOS): 4 = disco de Saturn ("SEGA SEGASATURN " no
    // setor 0), 2 = CD-ROM comum. O resultado é lido pelo comando 0xE1.
    void authenticate_disc();

    // Save state: registradores, posição, filtros, buffer, transferência e diretório. O leitor de disco não entra: os setores do
    // buffer são guardados só por FAD/partição e relidos do disco no load.
    void save(StateWriter& w) const;
    void load(StateReader& r);

    bool irq_pending() const { return (hirq_ & hirq_mask_) != 0; }
    const std::vector<uint8_t>& unknown_commands() const { return unknown_commands_; }

    // Bits de HIRQ.
    static constexpr uint16_t kCmok = 0x0001, kDrdy = 0x0002, kCsct = 0x0004, kBful = 0x0008, kPend = 0x0010, kEhst = 0x0080, kEfls = 0x0200, kScdq = 0x0400,
                              kMped = 0x0800;

private:
    struct Port;
    struct BufferedSector { uint32_t fad; int partition; std::array<uint8_t, saturnfs::kUserSectorSize> data; };
    struct Filter { uint32_t range_start = 0, range_count = 0; uint8_t mode = 0; uint8_t channel = 0, file_number = 0; uint16_t masks = 0, values = 0; int true_out = 0xFF, false_out = 0xFF; };
    struct Transfer { std::vector<uint8_t> bytes; size_t pos = 0; bool delete_on_end = false; int partition = 0; uint32_t start = 0, count = 0; };

    void execute();
    std::array<uint16_t, 4> status_reply() const;
    void finish(std::array<uint16_t, 4> reply, uint16_t hirq_bits = 0);
    void route_sector(uint32_t fad);
    void play(uint32_t start_fad, uint32_t length);
    void pump_play();
    void stop_play();
    void begin_transfer(std::vector<uint8_t> bytes);
    void load_directory(uint32_t file_id);
    void read_directory(uint32_t file_id);
    uint32_t visible_file_count() const;
    uint32_t sectors_in(int partition) const;
    // Remove os setores [start, start+count) da partição, contados dentro dela (count 0xFFFF = todos).
    void delete_sectors(int partition, uint32_t start, uint32_t count);
    void reset_selector(uint8_t flags);
    void software_reset();

    const saturnfs::SectorReader& disc_;
    std::unique_ptr<Port> port_;
    uint16_t hirq_ = 0x0BE1, hirq_mask_ = 0xFFFF;   // estado após o reset do drive (CMOK, DCHG, ESEL, EHST, ECPY, EFLS, MPCM) [inf]
    uint16_t cr_[4] = {};
    uint8_t status_ = 0x01;   // PAUSE
    uint8_t auth_type_ = 0;   // 0 = não autenticado
    uint32_t fad_ = 150;           // posição de leitura (próximo setor do Play)
    uint32_t play_remaining_ = 0;  // setores do Play ainda não entregues ao buffer
    uint32_t actual_words_ = 0;
    uint64_t subcode_cycles_ = 0, cycles_since_command_ = 0;
    int cd_connection_ = 0xFF;
    std::array<Filter, 24> filters_ = default_filters();   // saída verdadeira do filtro i = partição i
    static std::array<Filter, 24> default_filters();
    std::vector<BufferedSector> buffer_;
    Transfer transfer_;
    std::vector<saturnfs::DirEntry> directory_;
    uint32_t directory_first_id_ = 2;   // os IDs 0 e 1 são "." e ".."
    std::vector<uint8_t> unknown_commands_;
};

}  // namespace saturn
