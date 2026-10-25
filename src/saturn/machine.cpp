#include "saturn/machine.hpp"

#include <algorithm>
#include <cstring>

#include "saturnfs/iso9660.hpp"

namespace saturn {
namespace {

constexpr uint32_t kIpBase = 0x06002000, kIpEntry = 0x06002100;
constexpr uint32_t kDefaultIpSize = 0x1800, kIpSizeOffset = 0xE0, kFirstReadAddressOffset = 0xF0, kFirstReadSizeOffset = 0xF4;
constexpr uint32_t kBootFirstReadSlot = 0x000005EC;   // serviço da ROM da BIOS: carrega o 1º arquivo e salta para ele

uint32_t be32(const std::array<uint8_t, saturnfs::kUserSectorSize>& sector, size_t offset) {
    return (uint32_t(sector[offset]) << 24) | (uint32_t(sector[offset + 1]) << 16) | (uint32_t(sector[offset + 2]) << 8) | sector[offset + 3];
}
constexpr uint32_t kSlice = 64;  // ciclos entre atualizações do hardware em run_cycles

// Mapeamentos de E/S (endereços físicos).
constexpr uint32_t kSmpcBase = 0x00100000, kScuBase = 0x05FE0000, kVdp2RegistersBase = 0x05F80000, kRegionSize = 0x1000;
constexpr uint32_t kBackupRamBase = 0x00180000, kABusBase = 0x02000000, kABusSize = 0x03000000;
constexpr uint32_t kScspRamBase = 0x05A00000, kScspRegistersBase = 0x05B00000;
constexpr uint32_t kVdp1VramBase = 0x05C00000, kVdp1FramebufferBase = 0x05C80000, kVdp1RegistersBase = 0x05D00000;
constexpr uint32_t kVdp2VramBase = 0x05E00000, kVdp2CramBase = 0x05F00000;
constexpr uint32_t kKilobyte = 0x400;
constexpr uint16_t kVdp1EdsrDrawEnd = 0x0002, kVdp1ModrVersion = 0x1000;  // registradores de estado do VDP1 (desenho "sempre terminado")

}  // namespace

Machine::Machine(std::unique_ptr<saturnfs::SectorReader> disc) : disc_(std::move(disc)) {
    map_standard_memory(bus_);
    cd_ = std::make_unique<CdBlock>(*disc_);
    bus_.map_io(kSmpcBase, kRegionSize, smpc_);
    bus_.map_io(kScuBase, kRegionSize, scu_);
    bus_.map_io(kVdp2RegistersBase, kRegionSize, timing_);
    bus_.map_io(CdBlock::kRegisterBase, kRegionSize, *cd_);
    bus_.map_io(CdBlock::kRegisterBase + 0x8000, kRegionSize, *cd_);   // espelho dos registradores (o bit A15 não é decodificado)
    bus_.map_io(CdBlock::kDataPortBase, kRegionSize, cd_->data_port());
    // Memórias e registradores dos chips de vídeo/som: armazenamento simples até as etapas 3 e 4.
    bus_.map_ram(kBackupRamBase, 64 * kKilobyte, 0x80000);
    bus_.map_io(kABusBase, kABusSize, abus_);                       // A-bus sem cartucho: lê 0xFF
    bus_.map_io(kScspRamBase, kRegionSize, sound_mailbox_);              // 1ª página da RAM de som: caixa de correio do driver
    bus_.map_ram(kScspRamBase + kRegionSize, 512 * kKilobyte - kRegionSize, 512 * kKilobyte - kRegionSize);
    bus_.map_io(kScspRegistersBase, kRegionSize, scsp_regs_);
    bus_.map_ram(kVdp1VramBase, 512 * kKilobyte, 512 * kKilobyte);
    bus_.map_ram(kVdp1FramebufferBase, 256 * kKilobyte, 256 * kKilobyte);
    bus_.map_io(kVdp1RegistersBase, kRegionSize, vdp1_regs_);
    vdp1_regs_.set16(0x10, kVdp1EdsrDrawEnd);
    vdp1_regs_.set16(0x16, kVdp1ModrVersion);
    bus_.map_ram(kVdp2VramBase, 512 * kKilobyte, 512 * kKilobyte);
    bus_.map_ram(kVdp2CramBase, 4 * kKilobyte, 512 * kKilobyte);

    master_ = std::make_unique<sh2::Cpu>(bus_, 0);
    slave_ = std::make_unique<sh2::Cpu>(bus_, 1);
    master_->set_recorder(&recorder_);

    scu_.deliver = [this](int level, int vector) { master_->raise_interrupt(level, vector); };
    master_->on_interrupt_taken = [this](int level, int vector) {
        scu_.acknowledge(vector);
        if (on_interrupt_taken) on_interrupt_taken(level, vector);
    };
    smpc_.on_slave_power = [this](bool on) {
        slave_running_ = on;
        if (on && bios_rom_) {  // com a BIOS real o escravo parte do vetor de reset da ROM
            slave_->reset(bus_.read32(0), bus_.read32(4));
            slave_credit_ = 0;
        }
    };
    bios_.register_service(kBootFirstReadSlot, [this](sh2::Cpu& cpu) { load_first_read_file(cpu); });
}

Machine::~Machine() = default;

void Machine::boot_with_bios(std::span<const uint8_t> rom) {
    bus_.map_rom(0, rom);
    bios_rom_ = true;
    master_->reset(bus_.read32(0), bus_.read32(4));
}

void Machine::boot() {
    // O BIOS lê o IP (tamanho no cabeçalho, +0xE0) para 0x06002000 e salta para o código logo após o cabeçalho de 0x100 bytes.
    const auto header = disc_->read_user_sector(0);
    uint32_t ip_size = be32(header, kIpSizeOffset);
    if (ip_size < 0x100) ip_size = kDefaultIpSize;
    const uint32_t sectors = (ip_size + saturnfs::kUserSectorSize - 1) / saturnfs::kUserSectorSize;
    for (uint32_t i = 0; i < sectors; ++i) {
        const auto sector = disc_->read_user_sector(i);
        auto destination = bus_.ram_at(kIpBase + i * saturnfs::kUserSectorSize, saturnfs::kUserSectorSize);
        std::memcpy(destination.data(), sector.data(), sector.size());
    }

    master_->reset(kIpEntry, kIpBase);
    bios_.install(*master_);
}

// Serviço 0x5EC da BIOS: lê o primeiro arquivo da raiz (por convenção "0.BIN") para o endereço de leitura inicial do cabeçalho do IP
// (+0xF0; tamanho em +0xF4, 0 = o arquivo todo) e passa a execução a ele.
void Machine::load_first_read_file(sh2::Cpu& cpu) {
    const auto header = disc_->read_user_sector(0);
    const uint32_t address = be32(header, kFirstReadAddressOffset), limit = be32(header, kFirstReadSizeOffset);

    const auto root = saturnfs::root_directory(*disc_);
    for (const auto& entry : saturnfs::list_directory(*disc_, root.lba, root.size)) {
        if (entry.is_directory) continue;  // "." e ".." também são diretórios
        auto bytes = saturnfs::read_file(*disc_, {entry.name, entry.lba, entry.size});
        const size_t size = limit ? std::min<size_t>(limit, bytes.size()) : bytes.size();
        auto destination = bus_.ram_at(address, size);
        if (destination.size() != size) throw MachineError("leitura inicial fora da RAM de trabalho: " + std::to_string(address));
        std::memcpy(destination.data(), bytes.data(), size);
        cpu.state().pc = address;
        return;
    }
    throw MachineError("o disco não tem arquivo de leitura inicial");
}

void Machine::advance_hardware(uint32_t cycles) {
    timing_.advance(cycles);
    cd_->advance(cycles);
    master_->onchip().advance(cycles);
    if (slave_running_) slave_->onchip().advance(cycles);
}

uint32_t Machine::step_master() {
    uint32_t cycles = 1;
    if (bios_rom_ || !bios_.handle(*master_)) cycles = master_->step();
    advance_hardware(cycles);
    if (slave_running_ && bios_rom_) {  // o escravo recebe os mesmos ciclos do mestre
        slave_credit_ += cycles;
        while (slave_credit_ > 0) slave_credit_ -= slave_->step();
    }
    return cycles;
}

bool Machine::run_until_pc(uint32_t pc, uint64_t max_cycles) {
    for (uint64_t done = 0; done < max_cycles;) {
        if (master_->state().pc == pc) return true;
        done += step_master();
    }
    return master_->state().pc == pc;
}

void Machine::run_cycles(uint64_t cycles) {
    uint64_t done = 0;
    while (done < cycles) {
        uint32_t slice = 0;
        while (slice < kSlice) slice += step_master();
        done += slice;
    }
}

}  // namespace saturn
