#include "saturn/machine.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "saturnfs/iso9660.hpp"

namespace saturn {
namespace {

constexpr uint32_t kIpBase = 0x06002000, kIpEntry = 0x06002100;
constexpr uint32_t kDefaultIpSize = 0x1800, kIpSizeOffset = 0xE0, kFirstReadAddressOffset = 0xF0, kFirstReadSizeOffset = 0xF4;
constexpr uint32_t kBootFirstReadSlot = 0x000005EC;   // serviço da ROM da BIOS: carrega o 1º arquivo e salta para ele
constexpr uint32_t kBootFontSlot = 0x000005E4, kBootTextSlot = 0x000005E8;
constexpr uint32_t kBootFontRam = 0x06000E20, kBootFontVram = 0x25E00000, kBootFontSize = 0x1000;
constexpr uint32_t kTextVram = 0x25E06988, kTextFooterVram = 0x25E06B14;
constexpr uint32_t kBackupInitSlot = 0x06000358, kBackupDispatchPointer = 0x06000354;
constexpr uint32_t kBackupLibrarySize = 16 * 1024, kBackupWorkSize = 8 * 1024;
constexpr uint32_t kBackupMemoryAddress = 0x00180000, kBackupMemorySize = 0x8000, kBackupBlockSize = 0x40;

uint32_t be32(const std::array<uint8_t, saturnfs::kUserSectorSize>& sector, size_t offset) {
    return (uint32_t(sector[offset]) << 24) | (uint32_t(sector[offset + 1]) << 16) | (uint32_t(sector[offset + 2]) << 8) | sector[offset + 3];
}
constexpr uint32_t kSlice = 64;  // ciclos entre atualizações do hardware em run_cycles

// Mapeamentos de E/S (endereços físicos).
constexpr uint32_t kSmpcBase = 0x00100000, kScuBase = 0x05FE0000, kVdp2RegistersBase = 0x05F80000, kRegionSize = 0x1000;
constexpr uint32_t kSinitBase = 0x01000000, kMinitBase = 0x01800000, kABusBase = 0x02000000, kABusSize = 0x03000000;
constexpr uint32_t kScspRamBase = 0x05A00000, kScspRamSpan = 0x00100000, kScspRegistersBase = 0x05B00000;
constexpr uint32_t kVdp1VramBase = 0x05C00000, kVdp1FramebufferBase = 0x05C80000, kVdp1RegistersBase = 0x05D00000;
constexpr uint32_t kVdp2VramBase = 0x05E00000, kVdp2CramBase = 0x05F00000;
constexpr uint32_t kKilobyte = 0x400;
// HLE do escravo: ponteiro do ponto de entrada (preenchido pelo jogo) e pilha escolhida abaixo da fonte do boot (0x06000E20).
constexpr uint32_t kSlaveStackTop = 0x06001000;  // pilha padrão do escravo (IP +0xEC = 0)
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
    // Memórias e registradores dos chips de vídeo: armazenamento simples até a etapa 3; o SCSP tem modelo próprio.
    bus_.map_ram(kBackupMemoryAddress, kBackupRamSize, 0x80000);
    format_backup_memory_header();
    bus_.map_io(kABusBase, kABusSize, abus_);                       // A-bus sem cartucho: lê 0xFF
    bus_.map_io(kScspRamBase, kScspRamSpan, scsp_ram_);
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

    scu_.deliver = [this](int level, int vector) { master_->raise_interrupt(level, vector); };
    master_->on_interrupt_taken = [this](int level, int vector) {
        scu_.acknowledge(vector);
        if (on_interrupt_taken) on_interrupt_taken(level, vector);
    };
    bus_.map_io(kSinitBase, kRegionSize, slave_init_);
    bus_.map_io(kMinitBase, kRegionSize, master_init_);
    slave_init_.on_write = [this] { slave_->onchip().signal_input_capture(); };
    master_init_.on_write = [this] { master_->onchip().signal_input_capture(); };
    smpc_.on_slave_power = [this](bool on) {
        slave_running_ = on;
        slave_credit_ = 0;
        slave_fault_.clear();
        if (!on) return;
        if (bios_rom_) slave_->reset(bus_.read32(0), bus_.read32(4));  // com a BIOS real o escravo parte do vetor de reset da ROM
        else slave_awaiting_entry_ = true;                              // HLE: parte do ponto de entrada que o jogo registrar
    };
    smpc_.on_sound_power = [this](bool on) { scsp_.power(on); };
    smpc_.on_clock_change = [this] { scu_.reset(); };   // o SMPC já desligou o escravo e o som
    smpc_.on_nmi = [this] { master_->raise_interrupt(sh2::Cpu::kNmiLevel, sh2::Cpu::kNmiVector); };
    scsp_.on_main_interrupt = [this] { scu_.raise(Scu::Interrupt::SoundRequest); };
    bios_.register_service(kBootFirstReadSlot, [this](sh2::Cpu& cpu) { load_first_read_file(cpu); });
    bios_.register_service(kBootFontSlot, [this](sh2::Cpu&) { expand_boot_font(); });
    bios_.register_service(kBootTextSlot, [this](sh2::Cpu& cpu) { draw_boot_text(cpu); });
    bios_.register_service(0x06000284, [this](sh2::Cpu& cpu) { init_vdp2_color_ram(cpu); });
    bios_.register_service(kBackupInitSlot, [this](sh2::Cpu& cpu) { init_backup_library(cpu); });
    bios_.register_backup_service(3, [this](sh2::Cpu& cpu) { backup_status(cpu); });
    bios_.register_backup_service(5, [this](sh2::Cpu& cpu) { backup_read(cpu); });
}

Machine::~Machine() = default;

void Machine::boot_with_bios(std::span<const uint8_t> rom) {
    bus_.map_rom(0, rom);
    bios_rom_ = true;
    booted_ = true;
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

    booted_ = true;
    cd_->acknowledge_disc_change();  // o BIOS já tratou a troca de disco e autenticou o disco antes de entregar o controle
    cd_->authenticate_disc();
    master_->reset(kIpEntry, kIpBase);
    bios_.install(*master_);
}

void Machine::set_boot_font(std::span<const uint8_t> font) {
    if (font.size() != kBootFontSize) throw std::invalid_argument("a fonte do boot deve ter exatamente 4096 bytes");
    boot_font_.assign(font.begin(), font.end());
}

// BIOS japonesa 0x50EA/0x5000: o auxiliar 0x5000 lê 4096 bytes em RAM
// (0x06000E20) e expande os pares de bits para a VRAM. A etapa anterior,
// que prepara esses 4096 bytes, ainda não está reimplementada: a entrada
// deve ser uma captura da RAM já preparada pela BIOS (não um recorte da ROM).
// A tabela de pixels vem do endereço 0x5782 da BIOS japonesa estudada.
void Machine::expand_boot_font() {
    if (boot_font_.empty() && !boot_font_required_) return;
    if (boot_font_.size() != kBootFontSize)
        throw MachineError("BIOS 0x000005E4: fonte descompactada ausente (4096 bytes); use tools/extract_boot_font.py e --boot-font");
    auto font_ram = bus_.ram_at(kBootFontRam, kBootFontSize);
    if (font_ram.size() != kBootFontSize) throw MachineError("RAM da fonte do boot não mapeada");
    constexpr std::array<uint8_t, 4> pixels{0x00, 0x0D, 0xD0, 0xDD};
    for (uint32_t i = 0; i < kBootFontSize; ++i) {
        const uint8_t packed = boot_font_[i];
        font_ram[i] = packed;
        for (uint32_t pair = 0; pair < 4; ++pair)
            bus_.write8(kBootFontVram + i * 4 + pair, pixels[(packed >> (6 - 2 * pair)) & 3]);
    }
}

void Machine::init_vdp2_color_ram(sh2::Cpu& cpu) {
    const auto& s = cpu.state();
    const uint32_t dest = s.r[5];
    const uint32_t size = s.r[4];
    const uint16_t value = static_cast<uint16_t>(s.r[6] & 0xFFFF);
    if (size == 0) return;
    for (uint32_t i = 0; i < size; i += 2) {
        bus_.write16(dest + i, value);
    }
}

// BIOS 0x511C/0x50A0: desenha os dois trechos do texto principal em linhas de
// 20 e 12 bytes, e a mensagem complementar terminada em zero. Cada caractere
// vira dois índices de padrões de 16 bits, um em cada metade do plano (stride 0x80).
void Machine::draw_boot_text(sh2::Cpu& cpu) {
    const uint32_t initial = cpu.state().r[4], footer = cpu.state().r[5];
    const uint32_t offset = (bus_.read32(0x06000248) & 0x0800) ? 0x100 : 0;
    auto write_string = [&](uint32_t dest, uint32_t src, size_t limit, bool stop_at_zero) {
        for (size_t i = 0; i < limit; ++i) {
            const uint8_t character = bus_.read8(src + static_cast<uint32_t>(i));
            if (character == 0) return;
            bus_.write16(dest + static_cast<uint32_t>(2 * i), uint16_t(character * 2));
            bus_.write16(dest + 0x80 + static_cast<uint32_t>(2 * i), uint16_t(character * 2 + 1));
        }
        if (stop_at_zero) throw MachineError("texto complementar da BIOS sem terminador NUL");
    };
    write_string(kTextVram + offset, initial, 20, false);
    write_string(kTextVram + offset + 0x2A, initial + 20, 12, false);
    write_string(kTextFooterVram + offset, footer, 0x1000, true);
}

// BUP_Init(r4=libaddr, r5=workbuff, r6=BupConfig[3]). A BIOS extrai a biblioteca
// para r4 (16 KiB) e coloca a tabela de 12 ponteiros no início de r5 (8 KiB).
// O HLE publica somente ponteiros sintéticos: outras funções exigem reprodução
// específica da biblioteca e do formato de gravação da SRAM.
void Machine::init_backup_library(sh2::Cpu& cpu) {
    const auto& s = cpu.state();
    const uint32_t libaddr = s.r[4], workaddr = s.r[5], conf = s.r[6];
    if ((libaddr & 3) || (workaddr & 3) || (conf & 1) ||
        bus_.ram_at(libaddr, kBackupLibrarySize).size() != kBackupLibrarySize ||
        bus_.ram_at(workaddr, kBackupWorkSize).size() != kBackupWorkSize ||
        bus_.ram_at(conf, 3 * 4).size() != 3 * 4)
        throw MachineError("BUP_Init: endereços da biblioteca, trabalho ou configuração inválidos");

    // Todos os serviços posteriores continuam diagnosticáveis individualmente.
    for (uint32_t i = 0; i < BiosHle::kBackupFunctionCount; ++i)
        bus_.write32(workaddr + 4 * i, BiosHle::backup_trap_address(i));
    bus_.write32(kBackupDispatchPointer, workaddr);

    // BupConfig contém três pares (unit_id, partition) de 16 bits.
    // Memória interna presente; sem cartucho de backup nem interface serial.
    bus_.write16(conf, 1);
    bus_.write16(conf + 2, 1);
    for (uint32_t offset = 4; offset < 12; offset += 2) bus_.write16(conf + offset, 0);
}

std::span<uint8_t> Machine::backup_ram() { return bus_.ram_at(kBackupMemoryAddress, kBackupRamSize); }

void Machine::load_backup_ram(std::span<const uint8_t> image) {
    if (image.size() != kBackupRamSize) throw std::invalid_argument("memória de backup com tamanho inesperado");
    std::ranges::copy(image, backup_ram().begin());
    format_backup_memory_header();
}

// Bloco 0 com "BackUpRam Format", como sai de fábrica: o BIOS real recusa a memória sem isso e o jogo pede para inicializá-la.
// Os saves começam no bloco 2, então nada salvo é tocado.
void Machine::format_backup_memory_header() {
    static constexpr std::string_view kSignature = "BackUpRam Format";
    for (uint32_t i = 0; i < kBackupBlockSize; ++i)   // bytes lógicos nos endereços ímpares
        bus_.write8(kBackupMemoryAddress + 2 * i + 1, static_cast<uint8_t>(kSignature[i % kSignature.size()]));
}

// A SRAM contém 32 KiB lógicos em endereços físicos ímpares, com blocos de
// 64 bytes. Os dois primeiros blocos são reservados; início de save tem bit 7
// no primeiro byte lógico do bloco (formato documentado pelo driver de backup).
bool Machine::backup_has_saves() {
    for (uint32_t block = 2; block < kBackupMemorySize / kBackupBlockSize; ++block)
        if (bus_.read8(kBackupMemoryAddress + block * kBackupBlockSize * 2 + 1) & 0x80) return true;
    return false;
}

// BUP_Stat: suporte seguro para SRAM sem gravações e dispositivo desconectado.
// Ao encontrar saves, exige implementação do alocador antes de reportar espaço livre.
void Machine::backup_status(sh2::Cpu& cpu) {
    auto& s = cpu.state();
    if (s.r[4] != 0) { s.r[0] = 1; return; } // BUP_NON
    if (bus_.ram_at(s.r[6], 24).size() != 24)
        throw MachineError("BUP_Stat: endereço da estrutura BupStat inválido");
    if (backup_has_saves()) throw MachineError("BUP_Stat: cálculo de espaço com saves ainda não implementado");

    constexpr uint32_t free_blocks = kBackupMemorySize / kBackupBlockSize - 2;
    constexpr uint32_t free_bytes = free_blocks * (kBackupBlockSize - 6) - 30;
    const uint32_t remaining = s.r[5] >= free_bytes ? 0 : free_bytes - s.r[5];
    bus_.write32(s.r[6] + 0, kBackupMemorySize);
    bus_.write32(s.r[6] + 4, kBackupMemorySize / kBackupBlockSize);
    bus_.write32(s.r[6] + 8, kBackupBlockSize);
    bus_.write32(s.r[6] + 12, free_bytes);
    bus_.write32(s.r[6] + 16, free_blocks);
    bus_.write32(s.r[6] + 20, remaining / kBackupBlockSize);
    s.r[0] = 0;
}

// BUP_Read: dispositivo ausente ou arquivo inexistente numa SRAM vazia.
// Uma SRAM com dados precisa da implementação completa da leitura de blocos.
void Machine::backup_read(sh2::Cpu& cpu) {
    auto& s = cpu.state();
    if (s.r[4] != 0) { s.r[0] = 1; return; } // BUP_NON
    if (backup_has_saves()) throw MachineError("BUP_Read: leitura de saves ainda não implementada");
    s.r[0] = 5; // BUP_NOT_FOUND
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
    scu_.advance(cycles);
    cd_->advance(cycles);
    smpc_.advance(cycles);
    scsp_.advance(cycles, timing_.master_hz());
    master_->onchip().advance(cycles);
    if (slave_running_) slave_->onchip().advance(cycles);
}

// Uma instrução do mestre (ou um gatilho HLE) e do escravo, sem avançar o resto do hardware.
uint32_t Machine::execute_instruction() {
    if (on_before_master_step) on_before_master_step(master_->state());
    const uint32_t cycles = step_cpu(*master_);
    if (slave_running_) step_slave(cycles);
    return cycles;
}

uint32_t Machine::step_master() {
    const uint32_t cycles = execute_instruction();
    advance_hardware(cycles);
    return cycles;
}

// O BIOS real deixa o escravo esperando e salta para o ponto de entrada em 0x06000250 quando o jogo o registra.
// Com a BIOS real a CPU sempre executa; sem ela, um gatilho HLE conta como 1 ciclo.
uint32_t Machine::step_cpu(sh2::Cpu& cpu) {
    const bool hle = !bios_rom_ && BiosHle::is_trap(cpu.state().pc) && bios_.handle(cpu);  // teste barato antes da chamada
    return hle ? 1 : cpu.step();
}

void Machine::start_slave_when_ready() {
    const uint32_t entry = bus_.read32(BiosHle::kSlaveEntryPointer);
    if (entry == 0) return;
    slave_->reset(entry, kSlaveStackTop);
    slave_->state().vbr = master_->state().vbr;
    slave_awaiting_entry_ = false;
}

void Machine::step_slave(uint32_t master_cycles) {  // o escravo recebe os mesmos ciclos do mestre
    if (slave_awaiting_entry_) {
        start_slave_when_ready();
        if (slave_awaiting_entry_) return;
    }
    slave_credit_ += master_cycles;
    try {
        while (slave_credit_ > 0) slave_credit_ -= step_cpu(*slave_);
    } catch (const MachineError& e) {  // no hardware a exceção prenderia só o escravo; o mestre segue
        slave_fault_ = e.what();
        slave_running_ = false;
    }
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
        while (slice < kSlice) slice += execute_instruction();
        advance_hardware(slice);   // o hardware avança em fatias: as interrupções chegam com até kSlice ciclos de atraso
        done += slice;
    }
}

void Machine::run_frame() {
    const uint64_t start = frame();
    while (frame() == start) run_cycles(timing_.cycles_to_frame_end());
}

// ---- Save state
// Formato: cabeçalho (assinatura, versão, identificação do disco, modo de boot, tamanho e checksum do conteúdo) + conteúdo em seções
// (MACH, BUS, SH2M, SH2S, SCU, SMPC, VTIM, VDP1, CDBL, SCSP). Tudo little-endian; ver state.hpp.
namespace {

constexpr char kStateMagic[8] = {'D', 'X', 'P', 'C', 'S', 'A', 'V', 'E'};
constexpr uint32_t kStateVersion = 4;   // 2: DSP do SCU e autenticação do CD block; 3: DSP do SCSP; 4: campos XA do diretório
constexpr uint8_t kBootedFlag = 1, kBiosRomFlag = 2;

}  // namespace

struct Machine::StateHeader {
    uint32_t sector_count;
    std::array<uint8_t, 16> product;   // ID do produto e versão (bytes 0x20–0x2F do setor 0)
    uint64_t sector0_hash;
    uint8_t flags;
};

Machine::StateHeader Machine::state_header() const {
    const auto sector0 = disc_->read_user_sector(0);
    StateHeader h{disc_->sector_count(), {}, state_checksum(sector0), static_cast<uint8_t>((booted_ ? kBootedFlag : 0) | (bios_rom_ ? kBiosRomFlag : 0))};
    std::copy_n(sector0.begin() + 0x20, h.product.size(), h.product.begin());
    return h;
}

std::vector<uint8_t> Machine::save_payload() const {
    StateWriter w(last_state_size_);
    write_payload(w);
    return w.take();
}

void Machine::write_payload(StateWriter& w) const {
    w.section("MACH", [&] {
        w.boolean(slave_running_);
        w.boolean(slave_awaiting_entry_);
        w.i64(slave_credit_);
        w.text(slave_fault_);
    });
    w.section("BUS ", [&] { bus_.save(w); });
    w.section("SH2M", [&] { master_->save(w); });
    w.section("SH2S", [&] { slave_->save(w); });
    w.section("SCU ", [&] { scu_.save(w); });
    w.section("SMPC", [&] { smpc_.save(w); });
    w.section("VTIM", [&] { timing_.save(w); });
    w.section("VDP1", [&] { vdp1_regs_.save(w); });
    w.section("CDBL", [&] { cd_->save(w); });
    w.section("SCSP", [&] { scsp_.save(w); });
}

// Cabeçalho, depois o conteúdo direto no mesmo bloco; tamanho e checksum do conteúdo são preenchidos no fim (sem cópia de vários MB).
std::vector<uint8_t> Machine::save_state() const {
    const StateHeader h = state_header();
    StateWriter out(last_state_size_);
    out.raw({reinterpret_cast<const uint8_t*>(kStateMagic), sizeof kStateMagic});
    out.u32(kStateVersion);
    out.u32(h.sector_count);
    out.raw(h.product);
    out.u64(h.sector0_hash);
    out.u8(h.flags);
    const size_t size_at = out.size();
    out.u64(0);
    out.u64(0);
    const size_t payload_at = out.size();
    write_payload(out);
    const auto payload = out.written_since(payload_at);
    out.patch_u64(size_at, payload.size());
    out.patch_u64(size_at + 8, state_checksum(payload));
    last_state_size_ = out.size() + 64 * 1024;
    return out.take();
}

void Machine::apply_state(std::span<const uint8_t> payload) {
    StateReader r(payload);
    r.section("MACH", [&](StateReader& s) {
        slave_running_ = s.boolean();
        slave_awaiting_entry_ = s.boolean();
        slave_credit_ = s.i64();
        slave_fault_ = s.text();
    });
    r.section("BUS ", [&](StateReader& s) { bus_.load(s); });
    r.section("SH2M", [&](StateReader& s) { master_->load(s); });
    r.section("SH2S", [&](StateReader& s) { slave_->load(s); });
    r.section("SCU ", [&](StateReader& s) { scu_.load(s); });
    r.section("SMPC", [&](StateReader& s) { smpc_.load(s); });
    r.section("VTIM", [&](StateReader& s) { timing_.load(s); });
    r.section("VDP1", [&](StateReader& s) { vdp1_regs_.load(s); });
    r.section("CDBL", [&](StateReader& s) { cd_->load(s); });
    r.section("SCSP", [&](StateReader& s) { scsp_.load(s); });
    if (r.remaining() != 0) throw StateError("save state corrompido: dados após a última seção");
}

// Valida o cabeçalho e o checksum antes de tocar na máquina; se a aplicação falhar no meio (conteúdo com checksum válido mas
// incoerente), restaura o estado anterior.
void Machine::load_state(std::span<const uint8_t> state) {
    StateReader r(state);
    char magic[sizeof kStateMagic];
    r.raw({reinterpret_cast<uint8_t*>(magic), sizeof magic});
    if (std::memcmp(magic, kStateMagic, sizeof magic) != 0) throw StateError("não é um save state do dxpc");
    if (const uint32_t version = r.u32(); version != kStateVersion)
        throw StateError("versão de save state não suportada: " + std::to_string(version) + " (esperada " + std::to_string(kStateVersion) + ")");

    const StateHeader mine = state_header();
    StateHeader theirs{};
    theirs.sector_count = r.u32();
    r.raw(theirs.product);
    theirs.sector0_hash = r.u64();
    theirs.flags = r.u8();
    if (theirs.sector_count != mine.sector_count || theirs.product != mine.product || theirs.sector0_hash != mine.sector0_hash)
        throw StateError("o save state é de outro disco");
    if (theirs.flags != mine.flags)
        throw StateError("o save state foi feito em outro modo de boot (BIOS por software x ROM) ou a máquina ainda não foi inicializada");

    const uint64_t size = r.u64(), expected_checksum = r.u64();
    if (size != r.remaining()) throw StateError("save state truncado ou com sobra de dados");
    const auto payload = r.rest();
    if (state_checksum(payload) != expected_checksum) throw StateError("save state corrompido (checksum)");

    const std::vector<uint8_t> previous = save_payload();
    try {
        apply_state(payload);
    } catch (...) {
        apply_state(previous);
        throw;
    }
}

}  // namespace saturn
