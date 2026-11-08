#include "saturn/smpc.hpp"

namespace saturn {
namespace {

constexpr uint32_t kIreg0 = 0x01, kIreg1 = 0x03, kComreg = 0x1F, kOreg0 = 0x21, kOreg31 = 0x5F, kSr = 0x61, kSf = 0x63;
constexpr uint32_t kRegisterSpace = 0x80;

constexpr uint32_t oreg(int n) { return kOreg0 + 2 * n; }

enum Command : uint8_t {
    kMshOn = 0x00, kSshOn = 0x02, kSshOff = 0x03, kSndOn = 0x06, kSndOff = 0x07, kCdOn = 0x08, kCdOff = 0x09, kSysRes = 0x0D,
    kCkChg352 = 0x0E, kCkChg320 = 0x0F, kIntBack = 0x10, kSetTime = 0x16, kSetSmem = 0x17, kNmiReq = 0x18, kResEnab = 0x19, kResDisa = 0x1A
};

constexpr uint8_t kStatusValidTime = 0x80, kStatusResetDisabled = 0x40;   // OREG0 do INTBACK de status
// OREG10 (SYSTEM STATUS 1): DOTSEL (relógio 352), fixos em 1, SLAVE, SOUND. OREG11: CDRES.
constexpr uint8_t kStatus1Clock352 = 0x40, kStatus1Slave = 0x10, kStatus1Sound = 0x01, kStatus1AlwaysOn = 0x24 | 0x08 | 0x02;
constexpr uint8_t kStatus2Cd = 0x40, kStatus2AlwaysOn = 0x02;
// Relógio de tempo real fixo (determinismo): ano 1995 (BCD, 2 bytes), dia da semana/mês, dia, hora, minuto, segundo.
constexpr uint8_t kFixedTime[7] = {0x19, 0x95, 0x01, 0x01, 0x00, 0x00, 0x00};
constexpr uint8_t kPort1OneDevice = 0xF1, kPortEmpty = 0xF0, kDigitalPadTwoBytes = 0x02;
constexpr uint8_t kIntbackStatusRequest = 0x01, kIntbackPeripheralRequest = 0x08, kIntbackBreak = 0x40, kIntbackContinue = 0x80;
constexpr uint8_t kSrNextPeripheral = 0x40, kSrPeripheralData = 0x80;   // NPE: há mais dados; PDL: dados de periférico

}  // namespace

Smpc::Smpc(Scu& scu) : scu_(scu) {}

void Smpc::save(StateWriter& w) const {
    w.raw(regs_);
    w.u16(pad_);
    w.u8(area_code_);
    w.boolean(peripheral_pending_);
    w.u32(completion_cycles_);
    w.boolean(nmi_on_completion_);
    for (bool v : {clock_352_, slave_on_, sound_on_, cd_on_, reset_enabled_}) w.boolean(v);
    w.raw(smem_);
}

void Smpc::load(StateReader& r) {
    r.raw(regs_);
    pad_ = r.u16();
    area_code_ = r.u8();
    peripheral_pending_ = r.boolean();
    completion_cycles_ = r.u32();
    nmi_on_completion_ = r.boolean();
    for (bool* v : {&clock_352_, &slave_on_, &sound_on_, &cd_on_, &reset_enabled_}) *v = r.boolean();
    r.raw(smem_);
}

uint8_t Smpc::read_byte(uint32_t offset) const { return (offset & 1) && offset < kRegisterSpace ? regs_[offset] : 0; }

void Smpc::write_byte(uint32_t offset, uint8_t value) {
    if (!(offset & 1) || offset >= kRegisterSpace) return;  // só os bytes ímpares existem
    regs_[offset] = offset == kSf ? (value & 1) : value;
    if (offset == kComreg) execute(value);
    else if (offset == kIreg0 && peripheral_pending_) {   // continuação ou cancelamento de um INTBACK em andamento
        if (value & kIntbackBreak) peripheral_pending_ = false;
        else if (value & kIntbackContinue) { send_peripheral_data(); completion_cycles_ = kCommandCycles; }
    }
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
        case kSshOn: case kSshOff: set_slave(command == kSshOn); break;
        case kSndOn: case kSndOff: set_sound(command == kSndOn); break;
        case kCdOn: case kCdOff: cd_on_ = command == kCdOn; break;
        case kResEnab: case kResDisa: reset_enabled_ = command == kResEnab; break;
        case kSetSmem: for (int i = 0; i < 4; ++i) smem_[i] = regs_[kIreg0 + 2 * i]; break;
        case kIntBack: intback(); break;
        case kCkChg352: case kCkChg320:
            clock_352_ = command == kCkChg352;
            set_slave(false);   // a troca de relógio desliga o escravo e o som
            set_sound(false);
            if (on_clock_change) on_clock_change();
            nmi_on_completion_ = true;
            break;
        case kNmiReq: nmi_on_completion_ = true; break;
        case kMshOn: case kSysRes: case kSetTime:
            break;  // sem efeito observável nesta etapa
        default: unknown_commands_.push_back(command);
    }
    completion_cycles_ = (command == kCkChg352 || command == kCkChg320) ? kClockChangeCycles : kCommandCycles;
}

void Smpc::set_slave(bool on) {
    slave_on_ = on;
    if (on_slave_power) on_slave_power(on);
}

void Smpc::set_sound(bool on) {
    sound_on_ = on;
    if (on_sound_power) on_sound_power(on);
}

void Smpc::clear_oregs() {
    for (int i = 0; i < 31; ++i) regs_[oreg(i)] = 0;   // OREG31 guarda o eco do comando
}

void Smpc::advance(uint64_t cycles) {
    if (completion_cycles_ == 0) return;
    if (cycles < completion_cycles_) { completion_cycles_ -= static_cast<uint32_t>(cycles); return; }
    completion_cycles_ = 0;
    regs_[kSf] = 0;
    scu_.raise(Scu::Interrupt::SystemManager);
    if (nmi_on_completion_) {
        nmi_on_completion_ = false;
        if (on_nmi) on_nmi();
    }
}

// INTBACK: com o bit de status em IREG0 devolve o status do sistema (OREG0–15); senão, com o bit de periférico em IREG1, devolve o
// pad digital da porta 1. (Pedido simultâneo e continuação por IREG0.bit7 ainda não são tratados.) Formato dos dados: [inf].
void Smpc::intback() {
    const uint8_t ireg0 = regs_[kIreg0], ireg1 = regs_[kIreg1];
    clear_oregs();
    peripheral_pending_ = false;
    if (ireg0 & kIntbackStatusRequest) {
        regs_[oreg(0)] = kStatusValidTime | (reset_enabled_ ? 0 : kStatusResetDisabled);
        for (int i = 0; i < 7; ++i) regs_[oreg(1 + i)] = kFixedTime[i];
        regs_[oreg(9)] = area_code_;
        regs_[oreg(10)] = kStatus1AlwaysOn | (clock_352_ ? kStatus1Clock352 : 0) | (slave_on_ ? kStatus1Slave : 0) |
                          (sound_on_ ? kStatus1Sound : 0);
        regs_[oreg(11)] = kStatus2AlwaysOn | (cd_on_ ? kStatus2Cd : 0);
        for (int i = 0; i < 4; ++i) regs_[oreg(12 + i)] = smem_[i];
        peripheral_pending_ = ireg1 & kIntbackPeripheralRequest;
        regs_[kSr] = peripheral_pending_ ? kSrNextPeripheral : 0;
    } else if (ireg1 & kIntbackPeripheralRequest) {
        send_peripheral_data();
    }
}

void Smpc::send_peripheral_data() {
    clear_oregs();
    regs_[oreg(0)] = kPort1OneDevice;
    regs_[oreg(1)] = kDigitalPadTwoBytes;
    regs_[oreg(2)] = static_cast<uint8_t>(pad_ >> 8);
    regs_[oreg(3)] = static_cast<uint8_t>(pad_ & 0xFF);
    regs_[oreg(4)] = kPortEmpty;
    regs_[kSr] = kSrPeripheralData | kSrNextPeripheral;   // PDL (primeiro bloco) + NPE: o software encerra com BREAK (observado: o SGL exige os dois)
    peripheral_pending_ = false;
}

}  // namespace saturn
