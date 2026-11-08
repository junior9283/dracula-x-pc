#include "saturn/cdblock.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace saturn {
namespace {

constexpr uint32_t kDataTransferReg = 0x00, kHirqReg = 0x08, kHirqMaskReg = 0x0C, kCr1Reg = 0x18, kCr2Reg = 0x1C, kCr3Reg = 0x20, kCr4Reg = 0x24;
constexpr uint16_t kHirqEsel = 0x0040, kSoftwareResetHirq = 0x0800 | 0x0200 | 0x0100 | 0x0080 | kHirqEsel;
constexpr size_t kBufferSectors = 200;
constexpr uint32_t kDirectoryWindowEntries = 254; // 256 registros internos, com "." e ".." reservados
constexpr int kFilters = 24, kNoConnection = 0xFF;
constexpr uint32_t kFadOffset = 150;               // FAD = LBA + 150
constexpr uint32_t kRootFileId = 0xFFFFFF;
constexpr uint32_t kFadFlag = 0x800000, kFadMask = 0x7FFFFF;
constexpr uint8_t kFilterRangeMode = 0x40;
constexpr uint16_t kRejected = 0xFF00, kPeriodicFlag = 0x2000;   // PERI: bit 5 do byte de status em CR1
constexpr uint64_t kQuietAfterCommand = 100000;  // ≈ 3,5 ms: a resposta de um comando é lida bem antes
constexpr uint8_t kStatusPause = 0x01, kStatusPlay = 0x03;
constexpr int kTocEntries = 102;                   // 99 faixas + A0 (primeira) + A1 (última) + A2 (lead-out)
constexpr uint32_t kTocDataControl = 0x41000000;

enum Command : uint8_t {
    kGetStatus = 0x00, kGetHwInfo = 0x01, kGetToc = 0x02, kGetSessionInfo = 0x03, kInit = 0x04, kEndDataTransfer = 0x06, kPlay = 0x10, kSeek = 0x11,
    kSetConnection = 0x30, kGetConnection = 0x31, kSetFilterRange = 0x40, kSetFilterSubheader = 0x42, kGetFilterSubheader = 0x43, kSetFilterMode = 0x44, kSetFilterConnection = 0x46,
    kResetSelector = 0x48, kGetBufferSize = 0x50, kGetSectorNumber = 0x51, kCalcActualSize = 0x52, kGetActualSize = 0x53,
    kSetSectorLength = 0x60, kGetSectorData = 0x61, kDeleteSectorData = 0x62, kGetThenDelete = 0x63, kGetCopyError = 0x67,
    kChangeDirectory = 0x70, kReadDirectory = 0x71, kGetFileSystemScope = 0x72, kGetFileInfo = 0x73, kReadFile = 0x74, kAbortFile = 0x75,
    kAuthenticateDevice = 0xE0, kGetAuthenticationStatus = 0xE1
};

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

// Porta de dados (0x25818000): leitura sequencial do buffer de transferência corrente, big-endian; zeros depois do fim.
struct CdBlock::Port : IoDevice {
    explicit Port(Transfer& transfer) : transfer(transfer) {}
    uint32_t read(uint32_t, int bytes) override {
        uint32_t value = 0;
        for (int i = 0; i < bytes; ++i) {
            const size_t at = transfer.pos + i;
            value = (value << 8) | (at < transfer.bytes.size() ? transfer.bytes[at] : 0);
        }
        transfer.pos += bytes;
        return value;
    }
    void write(uint32_t, int, uint32_t) override {}
    Transfer& transfer;
};

CdBlock::CdBlock(const saturnfs::SectorReader& disc) : disc_(disc), port_(std::make_unique<Port>(transfer_)) {}
CdBlock::~CdBlock() = default;
IoDevice& CdBlock::data_port() { return *port_; }

void CdBlock::save(StateWriter& w) const {
    w.u16(hirq_);
    w.u16(hirq_mask_);
    for (uint16_t v : cr_) w.u16(v);
    w.u8(status_);
    w.u8(auth_type_);
    w.u32(fad_);
    w.u32(play_remaining_);
    w.u32(actual_words_);
    w.u64(subcode_cycles_);
    w.u64(cycles_since_command_);
    w.i32(cd_connection_);
    for (const Filter& f : filters_) {
        w.u32(f.range_start); w.u32(f.range_count); w.u8(f.mode); w.u8(f.channel); w.u8(f.file_number);
        w.u16(f.masks); w.u16(f.values); w.i32(f.true_out); w.i32(f.false_out);
    }
    w.u32(static_cast<uint32_t>(buffer_.size()));
    for (const BufferedSector& s : buffer_) { w.u32(s.fad); w.i32(s.partition); w.raw(s.data); }
    w.blob(transfer_.bytes);
    w.u64(transfer_.pos);
    w.boolean(transfer_.delete_on_end);
    w.i32(transfer_.partition);
    w.u32(transfer_.start);
    w.u32(transfer_.count);
    w.u32(static_cast<uint32_t>(directory_.size()));
    for (const auto& e : directory_) {
        w.text(e.name); w.u32(e.lba); w.u32(e.size); w.boolean(e.is_directory);
        w.u8(e.attributes); w.u8(e.unit_size); w.u8(e.gap_size); w.u8(e.file_number);
    }
    w.u32(directory_first_id_);
}

void CdBlock::load(StateReader& r) {
    hirq_ = r.u16();
    hirq_mask_ = r.u16();
    for (uint16_t& v : cr_) v = r.u16();
    status_ = r.u8();
    auth_type_ = r.u8();
    fad_ = r.u32();
    play_remaining_ = r.u32();
    actual_words_ = r.u32();
    subcode_cycles_ = r.u64();
    cycles_since_command_ = r.u64();
    // Conexões e partições indexam os filtros e o buffer: valores fora da faixa de um arquivo corrompido não podem entrar.
    const auto connection = [&r] {
        const int v = r.i32();
        if (v != kNoConnection && (v < 0 || v >= kFilters)) throw StateError("save state corrompido: conexão do CD block inválida");
        return v;
    };
    cd_connection_ = connection();
    for (Filter& f : filters_) {
        f.range_start = r.u32(); f.range_count = r.u32(); f.mode = r.u8(); f.channel = r.u8(); f.file_number = r.u8();
        f.masks = r.u16(); f.values = r.u16(); f.true_out = connection(); f.false_out = connection();
    }
    buffer_.clear();
    for (uint32_t n = r.count(kBufferSectors); n > 0; --n) {
        BufferedSector& s = buffer_.emplace_back();
        s.fad = r.u32();
        s.partition = r.i32();
        if (s.partition < 0 || s.partition >= kFilters) throw StateError("save state corrompido: partição do CD block inválida");
        r.raw(s.data);   // o conteúdo vai no estado: o load não depende de ler o disco
    }
    transfer_.bytes = r.blob();
    transfer_.pos = r.u64();
    if (transfer_.pos > transfer_.bytes.size()) throw StateError("save state corrompido: posição de transferência inválida");
    transfer_.delete_on_end = r.boolean();
    transfer_.partition = r.i32();
    transfer_.start = r.u32();
    transfer_.count = r.u32();
    directory_.clear();
    for (uint32_t n = r.count(1u << 20); n > 0; --n) {
        saturnfs::DirEntry e;
        e.name = r.text();
        e.lba = r.u32();
        e.size = r.u32();
        e.is_directory = r.boolean();
        e.attributes = r.u8();
        e.unit_size = r.u8();
        e.gap_size = r.u8();
        e.file_number = r.u8();
        directory_.push_back(std::move(e));
    }
    directory_first_id_ = r.u32();
}

// ---- Registradores: de 16 bits, espelhados nas duas metades de cada slot de 32 bits.
uint32_t CdBlock::read(uint32_t offset, int bytes) {
    uint16_t value = 0;
    switch (offset & ~3u) {
        case kDataTransferReg: return port_->read(0, bytes);  // DATATRNS (0x25898000): mesma fila da porta 0x25818000
        case kHirqReg: value = hirq_; break;
        case kHirqMaskReg: value = hirq_mask_; break;
        case kCr1Reg: value = cr_[0]; break;
        case kCr2Reg: value = cr_[1]; break;
        case kCr3Reg: value = cr_[2]; break;
        case kCr4Reg: value = cr_[3]; break;
        default: break;
    }
    if (bytes == 4) return (uint32_t(value) << 16) | value;
    if (bytes == 2) return value;
    return (offset & 1) ? (value & 0xFF) : (value >> 8);
}

void CdBlock::write(uint32_t offset, int bytes, uint32_t value) {
    uint16_t v = static_cast<uint16_t>(value);
    if (bytes == 1) {  // mescla o byte no valor atual do registrador
        const uint16_t current = static_cast<uint16_t>(read(offset & ~1u, 2));
        v = (offset & 1) ? (current & 0xFF00) | (value & 0xFF) : (current & 0x00FF) | uint16_t((value & 0xFF) << 8);
    }
    switch (offset & ~3u) {
        case kHirqReg: hirq_ &= v; break;            // escrever 0 limpa o bit
        case kHirqMaskReg: hirq_mask_ = v; break;
        case kCr1Reg: cr_[0] = v; break;
        case kCr2Reg: cr_[1] = v; break;
        case kCr3Reg: cr_[2] = v; break;
        case kCr4Reg: cr_[3] = v; execute(); break;  // CR4 dispara o comando
        default: break;
    }
}

void CdBlock::advance(uint64_t cycles) {
    subcode_cycles_ += cycles;
    cycles_since_command_ += cycles;
    if (subcode_cycles_ < kCyclesPerSubcodeFrame) return;
    subcode_cycles_ %= kCyclesPerSubcodeFrame;
    hirq_ |= kScdq;
    // Relatório periódico: o drive reescreve CR1–CR4 com o status (bit PERI), exceto logo após um comando, para não corromper sua resposta.
    if (cycles_since_command_ >= kQuietAfterCommand) {
        auto report = status_reply();
        report[0] |= kPeriodicFlag;
        std::copy(report.begin(), report.end(), cr_);
    }
}

std::array<uint16_t, 4> CdBlock::status_reply() const {
    return {static_cast<uint16_t>(status_ << 8), 0x4101, static_cast<uint16_t>((1u << 8) | ((fad_ >> 16) & 0xFF)), static_cast<uint16_t>(fad_ & 0xFFFF)};
}

void CdBlock::finish(std::array<uint16_t, 4> reply, uint16_t hirq_bits) {
    std::copy(reply.begin(), reply.end(), cr_);
    hirq_ |= kCmok | hirq_bits;
}

uint32_t CdBlock::sectors_in(int partition) const {
    return static_cast<uint32_t>(std::count_if(buffer_.begin(), buffer_.end(), [&](const BufferedSector& s) { return s.partition == partition; }));
}

void CdBlock::delete_sectors(int partition, uint32_t start, uint32_t count) {
    uint32_t index = 0;
    buffer_.erase(std::remove_if(buffer_.begin(), buffer_.end(), [&](const BufferedSector& s) {
        if (s.partition != partition) return false;
        const uint32_t i = index++;
        return i >= start && (count == 0xFFFF || i < start + count);
    }), buffer_.end());
}

// ---- Leitura de setores: cada setor entra pelo conector do CD e segue os filtros até uma partição do buffer.
void CdBlock::route_sector(uint32_t fad) {
    const auto data = disc_.read_user_sector(fad - kFadOffset);
    int filter = cd_connection_;
    for (int hops = 0; filter >= 0 && filter < kFilters && hops < kFilters; ++hops) {
        const Filter& f = filters_[filter];
        const bool pass = !(f.mode & kFilterRangeMode) || (fad >= f.range_start && fad < f.range_start + f.range_count);
        if (pass) {
            if (f.true_out < kFilters) buffer_.push_back({fad, f.true_out, data});
            return;
        }
        filter = f.false_out;
    }
}

void CdBlock::play(uint32_t start_fad, uint32_t length) {
    fad_ = start_fad;
    play_remaining_ = length;
    pump_play();
}

void CdBlock::stop_play() {
    play_remaining_ = 0;
    status_ = kStatusPause;
}

// Entrega ao buffer os setores que faltam do Play. Com o buffer cheio, para (BFUL, PAUSE) e retoma assim que o jogo liberar espaço.
void CdBlock::pump_play() {
    const uint32_t first_fad = fad_;
    status_ = kStatusPlay;
    while (play_remaining_ > 0) {
        if (fad_ < kFadOffset || fad_ - kFadOffset >= disc_.sector_count()) { play_remaining_ = 0; break; }  // fora do disco
        if (buffer_.size() >= kBufferSectors) { hirq_ |= kBful; status_ = kStatusPause; return; }
        route_sector(fad_++);
        --play_remaining_;
    }
    status_ = kStatusPause;
    hirq_ |= kPend | (fad_ != first_fad ? kCsct : 0);
}

void CdBlock::begin_transfer(std::vector<uint8_t> bytes) {
    transfer_.bytes = std::move(bytes);
    transfer_.pos = 0;
    hirq_ |= kDrdy;
}

void CdBlock::load_directory(uint32_t file_id) {
    saturnfs::DirEntry dir;
    if (file_id == kRootFileId) {
        dir = saturnfs::root_directory(disc_);
    } else {
        if (file_id >= directory_.size() || !directory_[file_id].is_directory) throw std::runtime_error("id de diretório inválido");
        dir = directory_[file_id];
    }
    directory_ = saturnfs::list_directory(disc_, dir.lba, dir.size);
    directory_first_id_ = 2;
}

uint32_t CdBlock::visible_file_count() const {
    if (directory_.size() <= directory_first_id_) return 0;
    return std::min<uint32_t>(kDirectoryWindowEntries, static_cast<uint32_t>(directory_.size() - directory_first_id_));
}

// 0x71 recarrega a janela de informações do diretório atual a partir do ID pedido;
// não entra em um subdiretório (isso é a operação 0x70).
void CdBlock::read_directory(uint32_t file_id) {
    if (file_id == 0) file_id = 2;  // início da enumeração, incluindo os registros reservados
    if (file_id < 2 || file_id >= directory_.size()) throw std::runtime_error("ID inicial de diretório inválido");
    directory_first_id_ = file_id;
}

void CdBlock::authenticate_disc() {
    static constexpr std::string_view kSaturnId = "SEGA SEGASATURN ";
    const auto sector = disc_.read_user_sector(0);
    auth_type_ = std::equal(kSaturnId.begin(), kSaturnId.end(), sector.begin()) ? 4 : 2;
}

std::array<CdBlock::Filter, 24> CdBlock::default_filters() {
    std::array<Filter, kFilters> filters{};
    for (int i = 0; i < kFilters; ++i) filters[i].true_out = i;
    return filters;
}

// Reset Selector com flags: 0x04 esvazia as partições, 0x10 zera as condições dos filtros, 0x20 desliga as entradas (conexão
// do CD e saídas falsas que apontam para filtros), 0x40 volta a saída verdadeira do filtro i para a partição i e 0x80 desliga
// as saídas falsas.
void CdBlock::reset_selector(uint8_t flags) {
    if (flags & 0x04) buffer_.clear();
    for (int i = 0; i < kFilters; ++i) {
        Filter& f = filters_[i];
        if (flags & 0x10) f = Filter{.true_out = f.true_out, .false_out = f.false_out};
        if (flags & 0x20) {
            if (cd_connection_ == i) cd_connection_ = kNoConnection;
            if (f.false_out < kFilters) f.false_out = kNoConnection;
        }
        if (flags & 0x40) f.true_out = i;
        if (flags & 0x80) f.false_out = kNoConnection;
    }
}

// Init com reset por software: filtros e conexões no estado de fábrica, buffer vazio.
void CdBlock::software_reset() {
    buffer_.clear();
    transfer_ = {};
    directory_.clear();
    filters_ = default_filters();
    cd_connection_ = kNoConnection;
}

// ---- Comandos
void CdBlock::execute() {
    cycles_since_command_ = 0;
    const std::array<uint16_t, 4> request = {cr_[0], cr_[1], cr_[2], cr_[3]};
    const uint16_t c1 = cr_[0], c2 = cr_[1], c3 = cr_[2], c4 = cr_[3];
    const uint8_t command = c1 >> 8;
    const int filter = c3 >> 8;
    const uint32_t file_id = (uint32_t(c3 & 0xFF) << 16) | c4;
    const auto status = [this] { return status_reply(); };

    try {
        // A raiz é o diretório corrente inicial: carrega-a antes de qualquer comando do sistema de arquivos (ids são relativos a ela).
        const bool file_system_command = command >= kChangeDirectory && command <= kAbortFile;
        if (directory_.empty() && file_system_command) load_directory(kRootFileId);

        switch (command) {
            case kGetStatus: finish(status()); break;
            case kGetHwInfo: finish({static_cast<uint16_t>(status_ << 8), 0x0201, 0, 0x0400}); break;
            case kGetToc: {
                // Disco de uma faixa de dados: faixa 1 em FAD 150, demais vazias, A0/A1 = faixa 1 e A2 = lead-out.
                std::vector<uint8_t> toc;
                for (int i = 0; i < kTocEntries; ++i) {
                    uint32_t entry = 0xFFFFFFFF;
                    if (i == 0) entry = kTocDataControl | kFadOffset;
                    else if (i == 99 || i == 100) entry = kTocDataControl | (1u << 16);
                    else if (i == 101) entry = kTocDataControl | (disc_.sector_count() + kFadOffset);
                    put_be32(toc, entry);
                }
                finish({static_cast<uint16_t>(status_ << 8), static_cast<uint16_t>(toc.size() / 2), 0, 0});
                begin_transfer(std::move(toc));
                break;
            }
            case kGetSessionInfo: {
                const uint32_t leadout = disc_.sector_count() + kFadOffset;
                if ((c1 & 0xFF) == 0) finish({static_cast<uint16_t>(status_ << 8), 0, static_cast<uint16_t>((1u << 8) | ((leadout >> 16) & 0xFF)), static_cast<uint16_t>(leadout & 0xFFFF)});
                else finish({static_cast<uint16_t>(status_ << 8), 0, static_cast<uint16_t>(1u << 8), 0});  // início da sessão 1 = FAD 0 (convenção do jogo: PVD = início + 0xA6)
                break;
            }
            case kInit:
                fad_ = kFadOffset;
                stop_play();
                // O reset por software termina ligando os bits de fim de todas as operações (MPED, EFLS, ECPY, EHST, ESEL).
                if (c1 & 0x01) { software_reset(); finish(status(), kSoftwareResetHirq); }
                else finish(status());
                break;
            case kEndDataTransfer: {
                const uint32_t words = static_cast<uint32_t>(transfer_.pos / 2);
                if (transfer_.delete_on_end) delete_sectors(transfer_.partition, transfer_.start, transfer_.count);
                transfer_ = {};
                finish({static_cast<uint16_t>((status_ << 8) | ((words >> 16) & 0xFF)), static_cast<uint16_t>(words & 0xFFFF), 0, 0}, kEhst);
                break;
            }
            case kPlay: {
                const uint32_t start = (uint32_t(c1 & 0xFF) << 16) | c2, end = (uint32_t(c3 & 0xFF) << 16) | c4;
                const uint32_t first = (start & kFadFlag) ? (start & kFadMask) : kFadOffset;     // modo faixa: faixa 1
                const uint32_t length = (end & kFadFlag) ? (end & kFadMask) : disc_.sector_count();  // FAD: nº de setores
                play(first, length);
                finish(status());
                break;
            }
            case kSeek: {
                const uint32_t start = (uint32_t(c1 & 0xFF) << 16) | c2;
                fad_ = (start & kFadFlag) ? (start & kFadMask) : kFadOffset;
                stop_play();
                finish(status());
                break;
            }
            case kSetConnection: cd_connection_ = filter; finish(status(), kHirqEsel); break;
            case kGetConnection: finish({static_cast<uint16_t>(status_ << 8), 0, static_cast<uint16_t>(cd_connection_ << 8), 0}); break;
            case kSetFilterRange:
                if (filter < kFilters) {
                    filters_[filter].range_start = (uint32_t(c1 & 0xFF) << 16) | c2;
                    filters_[filter].range_count = (uint32_t(c3 & 0xFF) << 16) | c4;
                }
                finish(status(), kHirqEsel);
                break;
            case kSetFilterSubheader:  // condições de subcabeçalho: guardadas; os setores de dados (modo 1) não têm subcabeçalho
                if (filter < kFilters) {
                    Filter& f = filters_[filter];
                    f.channel = c1 & 0xFF; f.masks = c2; f.file_number = c3 & 0xFF; f.values = c4;
                }
                finish(status(), kHirqEsel);
                break;
            case kGetFilterSubheader:
                if (filter >= kFilters) throw std::runtime_error("filtro inválido");
                finish({static_cast<uint16_t>((status_ << 8) | filters_[filter].channel), filters_[filter].values, filters_[filter].file_number, filters_[filter].masks});
                break;
            case kSetFilterMode: if (filter < kFilters) filters_[filter].mode = c1 & 0xFF; finish(status(), kHirqEsel); break;
            case kSetFilterConnection:
                if (filter < kFilters) {
                    if (c1 & 1) filters_[filter].true_out = c2 >> 8;
                    if (c1 & 2) filters_[filter].false_out = c2 & 0xFF;
                }
                finish(status(), kHirqEsel);
                break;
            case kResetSelector:
                if (c1 & 0xFF) reset_selector(static_cast<uint8_t>(c1));
                else delete_sectors(filter, 0, 0xFFFF);   // sem flags: só esvazia a partição
                finish(status(), kHirqEsel);
                break;
            case kGetBufferSize:
                finish({static_cast<uint16_t>(status_ << 8), 0x1800, static_cast<uint16_t>(kFilters << 8), static_cast<uint16_t>(kBufferSectors - buffer_.size())});
                break;
            case kGetSectorNumber: finish({static_cast<uint16_t>(status_ << 8), 0, 0, static_cast<uint16_t>(sectors_in(filter))}); break;
            case kCalcActualSize: {
                const uint32_t words = static_cast<uint32_t>(c4) * (saturnfs::kUserSectorSize / 2);
                actual_words_ = words;
                finish(status(), kHirqEsel);
                break;
            }
            case kGetActualSize:
                finish({static_cast<uint16_t>((status_ << 8) | ((actual_words_ >> 16) & 0xFF)), static_cast<uint16_t>(actual_words_ & 0xFFFF), 0, 0});
                break;
            case kSetSectorLength: finish(status(), kHirqEsel); break;
            case kGetSectorData:
            case kGetThenDelete: {
                std::vector<uint8_t> bytes;
                uint32_t index = 0;
                const uint32_t start = c2, count = c4;
                for (const auto& s : buffer_) {
                    if (s.partition != filter) continue;
                    if (index >= start && (count == 0xFFFF || index < start + count)) bytes.insert(bytes.end(), s.data.begin(), s.data.end());
                    ++index;
                }
                begin_transfer(std::move(bytes));
                transfer_.delete_on_end = command == kGetThenDelete;
                transfer_.partition = filter; transfer_.start = start; transfer_.count = count;
                finish(status());
                break;
            }
            case kDeleteSectorData: delete_sectors(filter, c2, c4); finish(status(), kEhst); break;
            case kChangeDirectory:
                if (filter >= kFilters) throw std::runtime_error("filtro de diretório inválido");
                load_directory(file_id);
                finish(status(), kEfls);
                break;
            case kReadDirectory:
                if (filter >= kFilters) throw std::runtime_error("filtro de diretório inválido");
                read_directory(file_id);
                finish(status(), kEfls);
                break;
            case kGetFileSystemScope: {
                const uint32_t count = visible_file_count();
                const bool last = directory_first_id_ + count >= directory_.size();
                finish({static_cast<uint16_t>(status_ << 8), static_cast<uint16_t>(count),
                        static_cast<uint16_t>((last ? 0x0100 : 0) | (directory_first_id_ >> 16)),
                        static_cast<uint16_t>(directory_first_id_ & 0xFFFF)}, kEfls);
                break;
            }
            case kGetFileInfo: {
                std::vector<uint8_t> info;
                const auto add = [&](uint32_t id) {
                    const auto& e = directory_.at(id);
                    put_be32(info, e.lba + kFadOffset);
                    put_be32(info, e.size);
                    info.push_back(e.unit_size);
                    info.push_back(e.gap_size);
                    info.push_back(e.file_number);   // número de arquivo XA
                    info.push_back(e.attributes);
                };
                const uint32_t count = visible_file_count();
                if (file_id == kRootFileId) {
                    for (uint32_t id = directory_first_id_; id < directory_first_id_ + count; ++id) add(id);
                } else {
                    // Os IDs 0 e 1 (diretório atual e pai) ficam sempre residentes,
                    // independentemente da janela de até 254 arquivos.
                    if (file_id >= directory_.size() ||
                        (file_id >= 2 && (file_id < directory_first_id_ || file_id >= directory_first_id_ + count)))
                        throw std::runtime_error("ID fora da janela de arquivos carregada");
                    add(file_id);
                }
                finish({static_cast<uint16_t>(status_ << 8), static_cast<uint16_t>(info.size() / 2), 0, 0});
                begin_transfer(std::move(info));
                break;
            }
            case kReadFile: {
                const auto& file = directory_.at(file_id);
                if (file.is_directory) throw std::runtime_error("ReadFile em diretório");
                const uint32_t offset = (uint32_t(c1 & 0xFF) << 16) | c2;
                const uint32_t sectors = (file.size + saturnfs::kUserSectorSize - 1) / saturnfs::kUserSectorSize;
                const uint32_t first = file.lba + kFadOffset + offset, count = sectors > offset ? sectors - offset : 0;
                if (filter < kFilters) { filters_[filter].range_start = first; filters_[filter].range_count = count; }
                cd_connection_ = filter;
                play(first, count);
                finish(status(), kEfls);
                break;
            }
            case kAbortFile: stop_play(); finish(status(), kEfls); break;   // o fim da operação de arquivo liga EFLS
            case kGetCopyError: finish({static_cast<uint16_t>(status_ << 8), 0, 0, 0}); break;   // cópia/movimento são síncronos: sem erro
            case kAuthenticateDevice:   // CR2 = 1: placa MPEG (ausente); 0: o disco
                if (c2 == 1) finish(status(), kMped);
                else { authenticate_disc(); finish(status(), kEfls); }
                break;
            case kGetAuthenticationStatus: finish({static_cast<uint16_t>(status_ << 8), c2 ? uint16_t{0} : uint16_t{auth_type_}, 0, 0}); break;
            default:
                unknown_commands_.push_back(command);
                finish({kRejected, 0, 0, 0});
        }
        if (play_remaining_ > 0) pump_play();  // o comando pode ter liberado espaço no buffer
    } catch (const std::exception&) {
        finish({kRejected, 0, 0, 0});  // parâmetro inválido (id inexistente, disco ilegível): comando rejeitado
    }
    if (on_command) on_command(request, {cr_[0], cr_[1], cr_[2], cr_[3]});
}

}  // namespace saturn
