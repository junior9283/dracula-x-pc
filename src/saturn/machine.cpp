#include "saturn/machine.hpp"

#include <algorithm>
#include <cstring>

namespace saturn {
namespace {

constexpr uint32_t kIpBase = 0x06002000, kIpEntry = 0x06002100;
constexpr uint32_t kDefaultIpSize = 0x1800, kIpSizeOffset = 0xE0;
constexpr uint32_t kSlice = 64;  // ciclos entre atualizações do hardware em run_cycles

// Mapeamentos de E/S (endereços físicos).
constexpr uint32_t kSmpcBase = 0x00100000, kScuBase = 0x05FE0000, kVdp2RegistersBase = 0x05F80000, kRegionSize = 0x1000;

}  // namespace

Machine::Machine(std::unique_ptr<saturnfs::SectorReader> disc) : disc_(std::move(disc)) {
    map_standard_memory(bus_);
    cd_ = std::make_unique<CdBlock>(*disc_);
    bus_.map_io(kSmpcBase, kRegionSize, smpc_);
    bus_.map_io(kScuBase, kRegionSize, scu_);
    bus_.map_io(kVdp2RegistersBase, kRegionSize, timing_);
    bus_.map_io(CdBlock::kRegisterBase, kRegionSize, *cd_);
    bus_.map_io(CdBlock::kDataPortBase, kRegionSize, cd_->data_port());

    master_ = std::make_unique<sh2::Cpu>(bus_, 0);
    slave_ = std::make_unique<sh2::Cpu>(bus_, 1);
    master_->set_recorder(&recorder_);

    scu_.deliver = [this](int level, int vector) { master_->raise_interrupt(level, vector); };
    master_->on_interrupt_taken = [this](int level, int vector) {
        scu_.acknowledge(vector);
        if (on_interrupt_taken) on_interrupt_taken(level, vector);
    };
    smpc_.on_slave_power = [this](bool on) { slave_running_ = on; };
}

Machine::~Machine() = default;

void Machine::boot() {
    // O BIOS lê o IP (tamanho no cabeçalho, +0xE0) para 0x06002000 e salta para o código logo após o cabeçalho de 0x100 bytes.
    const auto header = disc_->read_user_sector(0);
    uint32_t ip_size = (uint32_t(header[kIpSizeOffset]) << 24) | (uint32_t(header[kIpSizeOffset + 1]) << 16) |
                       (uint32_t(header[kIpSizeOffset + 2]) << 8) | header[kIpSizeOffset + 3];
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

void Machine::advance_hardware(uint32_t cycles) {
    timing_.advance(cycles);
    master_->onchip().advance(cycles);
    if (slave_running_) slave_->onchip().advance(cycles);
}

uint32_t Machine::step_master() {
    uint32_t cycles = 1;
    if (!bios_.handle(*master_)) cycles = master_->step();
    advance_hardware(cycles);
    return cycles;
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
