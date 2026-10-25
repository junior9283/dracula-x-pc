#include "saturn/smpc.hpp"

namespace saturn {
namespace {

constexpr uint32_t kIreg0 = 0x01, kIreg1 = 0x03, kComreg = 0x1F, kOreg0 = 0x21, kOreg31 = 0x5F, kSf = 0x63;
constexpr uint32_t kRegisterSpace = 0x80;

constexpr uint32_t oreg(int n) { return kOreg0 + 2 * n; }

enum Command : uint8_t {
    kMshOn = 0x00, kSshOn = 0x02, kSshOff = 0x03, kSndOn = 0x06, kSndOff = 0x07, kCdOn = 0x08, kCdOff = 0x09, kSysRes = 0x0D,
    kCkChg352 = 0x0E, kCkChg320 = 0x0F, kIntBack = 0x10, kSetTime = 0x16, kSetSmem = 0x17, kNmiReq = 0x18, kResEnab = 0x19, kResDisa = 0x1A
};

constexpr uint8_t kStatusValidTime = 0x80;       // OREG0 do INTBACK de status
constexpr uint8_t kPort1OneDevice = 0xF1, kPortEmpty = 0xF0, kDigitalPadTwoBytes = 0x02;
constexpr uint8_t kIntbackStatusRequest = 0x01, kIntbackPeripheralRequest = 0x08;

}  // namespace

Smpc::Smpc(Scu& scu) : scu_(scu) {}

uint8_t Smpc::read_byte(uint32_t offset) const { return (offset & 1) && offset < kRegisterSpace ? regs_[offset] : 0; }

void Smpc::write_byte(uint32_t offset, uint8_t value) {
    if (!(offset & 1) || offset >= kRegisterSpace) return;  // só os bytes ímpares existem
    regs_[offset] = offset == kSf ? (value & 1) : value;
    if (offset == kComreg) execute(value);
}

uint32_t Smpc::read(uint32_t offset, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | read_byte(offset + i);
    return value;
}

void Smpc::write(uint32_t offset, int bytes, uint32_t value) {
    for (int i = 0; i < bytes; ++i) write_byte(offset + i, (value >> (8 * (bytes - 1 - i))) & 0xFF);
}

void Smpc::execute(uint8_t command) {
    regs_[kOreg31] = command;  // eco do comando
    switch (command) {
        case kSshOn: if (on_slave_power) on_slave_power(true); break;
        case kSshOff: if (on_slave_power) on_slave_power(false); break;
        case kSndOn: if (on_sound_power) on_sound_power(true); break;
        case kSndOff: if (on_sound_power) on_sound_power(false); break;
        case kIntBack: intback(); break;
        case kMshOn: case kCdOn: case kCdOff: case kSysRes: case kCkChg352: case kCkChg320:
        case kSetTime: case kSetSmem: case kNmiReq: case kResEnab: case kResDisa:
            break;  // sem efeito observável nesta etapa
        default: unknown_commands_.push_back(command);
    }
    regs_[kSf] = 0;
    scu_.raise(Scu::Interrupt::SystemManager);
}

// INTBACK: com o bit de status em IREG0 devolve o status do sistema (OREG0–15); senão, com o bit de periférico em IREG1, devolve o
// pad digital da porta 1. (Pedido simultâneo e continuação por IREG0.bit7 ainda não são tratados.) Formato dos dados: [inf].
void Smpc::intback() {
    const uint8_t ireg0 = regs_[kIreg0], ireg1 = regs_[kIreg1];
    for (int i = 0; i < 31; ++i) regs_[oreg(i)] = 0;
    if (ireg0 & kIntbackStatusRequest) {
        regs_[oreg(0)] = kStatusValidTime;
        regs_[oreg(9)] = area_code_;
    } else if (ireg1 & kIntbackPeripheralRequest) {
        regs_[oreg(0)] = kPort1OneDevice;
        regs_[oreg(1)] = kDigitalPadTwoBytes;
        regs_[oreg(2)] = static_cast<uint8_t>(pad_ >> 8);
        regs_[oreg(3)] = static_cast<uint8_t>(pad_ & 0xFF);
        regs_[oreg(4)] = kPortEmpty;
    }
}

}  // namespace saturn
