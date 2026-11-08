#pragma once
#include <functional>
#include <memory>
#include <string>
#include <span>
#include <vector>

#include "saturn/bios_hle.hpp"
#include "saturn/bus.hpp"
#include "saturn/cdblock.hpp"
#include "saturn/devices.hpp"
#include "saturn/scu.hpp"
#include "saturn/smpc.hpp"
#include "saturn/state.hpp"
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
    // Memória de backup interna (saves): 64 KiB físicos em 0x00180000 (32 KiB lógicos nos bytes ímpares).
    static constexpr uint32_t kBackupRamSize = 0x10000;
    std::span<uint8_t> backup_ram();
    // Restaura a memória de backup (tamanho kBackupRamSize) e garante o cabeçalho de formatação: arquivos antigos podem não tê-lo.
    void load_backup_ram(std::span<const uint8_t> image);
    // Boot com a BIOS real (só para desenvolvimento/validação): mapeia a ROM em 0 e reseta o mestre pelos vetores de reset.
    void boot_with_bios(std::span<const uint8_t> rom);
    // Imagem de 4096 bytes da RAM da fonte, após a preparação pela BIOS do usuário.
    // A rotina de preparação da imagem ainda precisa de engenharia reversa.
    void set_boot_font(std::span<const uint8_t> font);
    // Sem a fonte (que vem da BIOS do usuário), o padrão é parar com erro; o port pode seguir com o texto do boot em branco.
    void set_boot_font_required(bool required) { boot_font_required_ = required; }
    // Executa até o PC do mestre valer `pc` (devolve true) ou até `max_cycles` (false).
    bool run_until_pc(uint32_t pc, uint64_t max_cycles);
    // Executa uma instrução do mestre (tratando os gatilhos do BIOS), avança o hardware e devolve os ciclos gastos.
    uint32_t step_master();
    void run_cycles(uint64_t cycles);

    sh2::Cpu& master() { return *master_; }
    sh2::Cpu& slave() { return *slave_; }
    Bus& bus() { return bus_; }
    Scu& scu() { return scu_; }
    Smpc& smpc() { return smpc_; }
    audio::Scsp& scsp() { return scsp_; }
    CdBlock& cd() { return *cd_; }
    sh2::ExecutionRecorder& recorder() { return recorder_; }
    // Grava os endereços executados pelo mestre (diagnóstico; desligado por padrão por custar ~6% do passo).
    void record_execution(bool on) { master_->set_recorder(on ? &recorder_ : nullptr); }
    bool slave_running() const { return slave_running_; }
    // Mensagem da exceção que parou o escravo (HLE); vazia se ele não falhou desde o último SSHON.
    const std::string& slave_fault() const { return slave_fault_; }
    void set_bios_strict(bool strict) { bios_.set_strict(strict); }
    const std::vector<BiosHle::Call>& bios_calls() const { return bios_.calls(); }
    uint64_t frame() const { return timing_.frame(); }
    const VideoTiming& timing() const { return timing_; }
    // Executa até o início do próximo quadro (fim do VBlank).
    void run_frame();
    // Duração real do quadro no modo de vídeo atual (NTSC: ~59,77 Hz em 320 pontos, ~59,83 Hz em 352).
    uint64_t frame_ns() const { return timing_.frame_ns(); }

    // Save state: o estado completo da máquina (CPUs, memórias, dispositivos, CD) num bloco de bytes versionado. load_state lança
    // std::runtime_error se o bloco for de outra versão/disco ou estiver corrompido, sem alterar a máquina.
    std::vector<uint8_t> save_state() const;
    void load_state(std::span<const uint8_t> state);

    // Observador das interrupções atendidas pelo mestre (nível, vetor).
    std::function<void(int level, int vector)> on_interrupt_taken;
    // Observador opcional chamado antes de cada passo do mestre, inclusive gatilhos HLE.
    // Serve a diagnósticos sem alterar a execução quando desativado.
    std::function<void(const sh2::Sh2State&)> on_before_master_step;

private:
    void format_backup_memory_header();
    struct StateHeader;
    StateHeader state_header() const;
    std::vector<uint8_t> save_payload() const;
    void write_payload(StateWriter& w) const;
    mutable size_t last_state_size_ = 0;   // reserva para o próximo save (evita realocações de um bloco de ~4 MB)
    void apply_state(std::span<const uint8_t> payload);
    uint32_t execute_instruction();
    void advance_hardware(uint32_t cycles);
    uint32_t step_cpu(sh2::Cpu& cpu);
    void start_slave_when_ready();
    void step_slave(uint32_t master_cycles);
    void load_first_read_file(sh2::Cpu& cpu);
    void expand_boot_font();
    void draw_boot_text(sh2::Cpu& cpu);
    void init_vdp2_color_ram(sh2::Cpu& cpu);
    void init_backup_library(sh2::Cpu& cpu);
    void backup_status(sh2::Cpu& cpu);
    void backup_read(sh2::Cpu& cpu);
    bool backup_has_saves();

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
    bool slave_awaiting_entry_ = false;  // HLE: SSHON recebido, falta o jogo registrar o ponto de entrada
    InitSignal master_init_, slave_init_;  // MINIT / SINIT
    bool bios_rom_ = false;
    bool booted_ = false;   // boot() ou boot_with_bios() já foi chamado (o save state exige o mesmo modo)
    std::vector<uint8_t> boot_font_;
    bool boot_font_required_ = true;
    int64_t slave_credit_ = 0;
    std::string slave_fault_;
    StorageDevice vdp1_regs_{0x100};
    audio::Scsp scsp_;
    ScspRamWindow scsp_ram_{scsp_};
    ScspRegisterWindow scsp_regs_{scsp_};
    ConstantDevice abus_{0xFF};
};

}  // namespace saturn
