#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "saturn/bus.hpp"
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

    bool irq_pending() const { return (hirq_ & hirq_mask_) != 0; }
    const std::vector<uint8_t>& unknown_commands() const { return unknown_commands_; }

    // Bits de HIRQ.
    static constexpr uint16_t kCmok = 0x0001, kDrdy = 0x0002, kCsct = 0x0004, kBful = 0x0008, kPend = 0x0010, kEhst = 0x0080, kEfls = 0x0200;

private:
    struct Port;
    struct BufferedSector { uint32_t fad; int partition; std::array<uint8_t, saturnfs::kUserSectorSize> data; };
    struct Filter { uint32_t range_start = 0, range_count = 0; uint8_t mode = 0; int true_out = 0xFF, false_out = 0xFF; };
    struct Transfer { std::vector<uint8_t> bytes; size_t pos = 0; bool delete_on_end = false; int partition = 0; uint32_t start = 0, count = 0; };

    void execute();
    std::array<uint16_t, 4> status_reply() const;
    void finish(std::array<uint16_t, 4> reply, uint16_t hirq_bits = 0);
    void route_sector(uint32_t fad);
    void play(uint32_t start_fad, uint32_t length);
    void begin_transfer(std::vector<uint8_t> bytes);
    void load_directory(uint32_t file_id);
    uint32_t sectors_in(int partition) const;
    // Remove os setores [start, start+count) da partição, contados dentro dela (count 0xFFFF = todos).
    void delete_sectors(int partition, uint32_t start, uint32_t count);

    const saturnfs::SectorReader& disc_;
    std::unique_ptr<Port> port_;
    uint16_t hirq_ = 0, hirq_mask_ = 0;
    uint16_t cr_[4] = {};
    uint8_t status_ = 0x01;   // PAUSE
    uint32_t fad_ = 150;
    uint32_t actual_words_ = 0;
    int cd_connection_ = 0xFF;
    std::array<Filter, 24> filters_;
    std::vector<BufferedSector> buffer_;
    Transfer transfer_;
    std::vector<saturnfs::DirEntry> directory_;
    std::vector<uint8_t> unknown_commands_;
};

}  // namespace saturn
