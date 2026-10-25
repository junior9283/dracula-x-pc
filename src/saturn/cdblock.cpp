#include "saturn/cdblock.hpp"

#include <algorithm>
#include <stdexcept>

namespace saturn {
namespace {

constexpr uint32_t kHirqReg = 0x08, kHirqMaskReg = 0x0C, kCr1Reg = 0x18, kCr2Reg = 0x1C, kCr3Reg = 0x20, kCr4Reg = 0x24;
constexpr uint16_t kHirqEsel = 0x0040;
constexpr size_t kBufferSectors = 200;
constexpr int kFilters = 24, kNoConnection = 0xFF;
constexpr uint32_t kFadOffset = 150;               // FAD = LBA + 150
constexpr uint32_t kRootFileId = 0xFFFFFF;
constexpr uint32_t kFadFlag = 0x800000, kFadMask = 0x7FFFFF;
constexpr uint8_t kFilterRangeMode = 0x40;
constexpr uint16_t kRejected = 0xFF00;
constexpr uint8_t kStatusPause = 0x01, kStatusPlay = 0x03;
constexpr size_t kInfoBytes = 12;                  // bytes por arquivo no GetFileInfo

enum Command : uint8_t {
    kGetStatus = 0x00, kGetHwInfo = 0x01, kInit = 0x04, kEndDataTransfer = 0x06, kPlay = 0x10, kSeek = 0x11,
    kSetConnection = 0x30, kGetConnection = 0x31, kSetFilterRange = 0x40, kSetFilterMode = 0x44, kSetFilterConnection = 0x46,
    kResetSelector = 0x48, kGetBufferSize = 0x50, kGetSectorNumber = 0x51, kCalcActualSize = 0x52, kGetActualSize = 0x53,
    kSetSectorLength = 0x55, kGetSectorData = 0x56, kDeleteSectorData = 0x57, kGetThenDelete = 0x58,
    kChangeDirectory = 0x60, kReadDirectory = 0x61, kGetFileSystemScope = 0x62, kGetFileInfo = 0x63, kReadFile = 0x64, kAbortFile = 0x65
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

// ---- Registradores: de 16 bits, espelhados nas duas metades de cada slot de 32 bits.
uint32_t CdBlock::read(uint32_t offset, int bytes) {
    uint16_t value = 0;
    switch (offset & ~3u) {
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
    status_ = kStatusPlay;
    uint32_t done = 0;
    for (; done < length; ++done) {
        const uint32_t fad = start_fad + done;
        if (fad < kFadOffset || fad - kFadOffset >= disc_.sector_count()) break;  // fora do disco
        if (buffer_.size() >= kBufferSectors) { hirq_ |= kBful; break; }
        route_sector(fad);
    }
    fad_ = start_fad + done;
    status_ = kStatusPause;
    hirq_ |= kPend | (done > 0 ? kCsct : 0);
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
}

// ---- Comandos
void CdBlock::execute() {
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
            case kInit:
                if (c1 & 0x01) { buffer_.clear(); filters_ = {}; cd_connection_ = kNoConnection; }
                fad_ = kFadOffset;
                status_ = kStatusPause;
                finish(status());
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
                status_ = kStatusPause;
                finish(status());
                break;
            }
            case kSetConnection: cd_connection_ = filter; finish(status()); break;
            case kGetConnection: finish({static_cast<uint16_t>(status_ << 8), 0, static_cast<uint16_t>(cd_connection_ << 8), 0}); break;
            case kSetFilterRange:
                if (filter < kFilters) {
                    filters_[filter].range_start = (uint32_t(c1 & 0xFF) << 16) | c2;
                    filters_[filter].range_count = (uint32_t(c3 & 0xFF) << 16) | c4;
                }
                finish(status(), kHirqEsel);
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
                if (c1 & 0x80) { buffer_.clear(); filters_ = {}; }
                else {
                    buffer_.erase(std::remove_if(buffer_.begin(), buffer_.end(), [&](const BufferedSector& s) { return s.partition == filter; }), buffer_.end());
                }
                finish(status(), kHirqEsel);
                break;
            case kGetBufferSize:
                finish({static_cast<uint16_t>(status_ << 8), 0x1800, static_cast<uint16_t>(kFilters << 8), static_cast<uint16_t>(kBufferSectors - buffer_.size())});
                break;
            case kGetSectorNumber: finish({static_cast<uint16_t>(status_ << 8), 0, 0, static_cast<uint16_t>(sectors_in(filter))}); break;
            case kCalcActualSize: {
                const uint32_t words = static_cast<uint32_t>(c4) * (saturnfs::kUserSectorSize / 2);
                actual_words_ = words;
                finish(status());
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
            case kReadDirectory: load_directory(file_id); finish(status(), kEfls); break;
            case kGetFileSystemScope:
                finish({static_cast<uint16_t>(status_ << 8), static_cast<uint16_t>(directory_.size()), 0x0100, 2});
                break;
            case kGetFileInfo: {
                std::vector<uint8_t> info;
                const auto add = [&](uint32_t id) {
                    const auto& e = directory_.at(id);
                    put_be32(info, e.lba + kFadOffset);
                    put_be32(info, e.size);
                    info.push_back(0);                               // unidade
                    info.push_back(0);                               // intervalo
                    info.push_back(static_cast<uint8_t>(id));        // número do arquivo
                    info.push_back(e.is_directory ? 2 : 0);          // atributo
                };
                if (file_id == kRootFileId) for (uint32_t id = 2; id < directory_.size(); ++id) add(id);
                else add(file_id);
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
            case kAbortFile: status_ = kStatusPause; finish(status()); break;
            default:
                unknown_commands_.push_back(command);
                finish({kRejected, 0, 0, 0});
        }
    } catch (const std::exception&) {
        finish({kRejected, 0, 0, 0});  // parâmetro inválido (id inexistente, disco ilegível): comando rejeitado
    }
}

}  // namespace saturn
