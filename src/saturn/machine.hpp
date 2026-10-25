#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "saturn/bios_hle.hpp"
#include "saturn/bus.hpp"
#include "saturn/cdblock.hpp"
#include "saturn/scu.hpp"
#include "saturn/smpc.hpp"
#include "saturn/video_timing.hpp"
#include "saturnfs/sector_reader.hpp"
#include "sh2/cpu.hpp"
#include "sh2/trace.hpp"

namespace saturn {

// Máquina completa: dois SH-2, barramento, SCU, SMPC, CD block, tempo de vídeo e o BIOS por software. Boot por HLE: carrega o IP
// do disco em 0x06002000 e entra nele como a BIOS faria.
class Machine {
public:
    explicit Machine(std::unique_ptr<saturnfs::SectorReader> disc);
    ~Machine();

    void boot();
    // Executa uma instrução do mestre (tratando os gatilhos do BIOS), avança o hardware e devolve os ciclos gastos.
    uint32_t step_master();
    void run_cycles(uint64_t cycles);

    sh2::Cpu& master() { return *master_; }
    sh2::Cpu& slave() { return *slave_; }
    Bus& bus() { return bus_; }
    Scu& scu() { return scu_; }
    Smpc& smpc() { return smpc_; }
    CdBlock& cd() { return *cd_; }
    sh2::ExecutionRecorder& recorder() { return recorder_; }
    bool slave_running() const { return slave_running_; }
    void set_bios_strict(bool strict) { bios_.set_strict(strict); }
    const std::vector<BiosHle::Call>& bios_calls() const { return bios_.calls(); }
    uint64_t frame() const { return timing_.frame(); }

    // Observador das interrupções atendidas pelo mestre (nível, vetor).
    std::function<void(int level, int vector)> on_interrupt_taken;

private:
    void advance_hardware(uint32_t cycles);
    void load_first_read_file(sh2::Cpu& cpu);

    std::unique_ptr<saturnfs::SectorReader> disc_;
    Bus bus_;
    Scu scu_{bus_};
    VideoTiming timing_{scu_};
    Smpc smpc_{scu_};
    std::unique_ptr<CdBlock> cd_;
    std::unique_ptr<sh2::Cpu> master_, slave_;
    BiosHle bios_{bus_, scu_};
    sh2::ExecutionRecorder recorder_;
    bool slave_running_ = false;
};

}  // namespace saturn
