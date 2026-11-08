#include "audio/scsp.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace audio {
namespace {

constexpr uint32_t kRamMask = Scsp::kRamSize - 1, kRegisterMask = Scsp::kRegisterSize - 1;
constexpr uint32_t kM68kRegisterBase = 0x100000, kM68kAddressMask = 0xFFFFFF;
constexpr int kTimerPrescalerShift = 8, kTimerPrescalerMask = 7;   // TACTL = bits 10–8 do registrador do timer

// Registradores de cada slot (0x20 bytes): controle (KYONEX bit 12, KYONB bit 11, LPCTL bits 6–5, SA bits 3–0), SA baixo, início/fim
// do laço, taxas do envelope (D2R/D1R/AR), KRS/DL/RR, TL (com SDIR), altura (OCT/FNS) e saída direta (DISDL/DIPAN).
constexpr uint32_t kSlotStride = 0x20, kSlotStartLow = 0x02, kSlotLoopStart = 0x04, kSlotLoopEnd = 0x06, kSlotRates = 0x08,
                   kSlotRelease = 0x0A, kSlotLevel = 0x0C, kSlotPitch = 0x10, kSlotInput = 0x14, kSlotOutput = 0x16;
constexpr uint32_t kMasterVolume = 0x400;   // MVOL: bits 3–0
constexpr uint16_t kKeyOnExecute = 1 << 12, kKeyOnBit = 1 << 11, kLoopControlMask = 3 << 5;
constexpr int kPositionFractionBits = 20, kMonitorSlotShift = 11, kMonitorAddressShift = 7, kCallAddressShift = 12;

// OCT: 4 bits com sinal (bits 14–11 da palavra de altura).
int slot_octave(uint16_t pitch) {
    const int octave = (pitch >> 11) & 0xF;
    return octave >= 8 ? octave - 16 : octave;
}

bool is_interrupt_register(uint32_t word_offset) { return word_offset >= Scsp::kScieb && word_offset <= Scsp::kMcire; }

// Ganhos em ponto fixo Q16 (65536 = 0 dB).
struct GainTables {
    uint32_t envelope[1024];   // atenuação de 10 bits, 0,09375 dB por passo: 2^(-n/64); 0x3FF = mudo
    uint32_t pan[16];          // DIPAN: 3 dB por passo, 15 = mudo
    uint32_t master[16];       // MVOL: 3 dB por passo abaixo de 15, 0 = mudo
    GainTables() {
        for (int i = 0; i < 1024; ++i) envelope[i] = i >= 0x3FF ? 0 : uint32_t(std::lround(65536.0 * std::exp2(-i / 64.0)));
        for (int i = 0; i < 16; ++i) {
            pan[i] = i == 15 ? 0 : uint32_t(std::lround(65536.0 * std::pow(10.0, -3.0 * i / 20.0)));
            master[i] = i == 0 ? 0 : uint32_t(std::lround(65536.0 * std::pow(10.0, -3.0 * (15 - i) / 20.0)));
        }
    }
};
const GainTables kGain;

// Passo do envelope para uma taxa efetiva de 0 a 63: grupos de 4 taxas dobram a frequência de atualização; padrões de 8 passos.
int envelope_increment(int rate, uint32_t counter) {
    static constexpr uint8_t kPattern[4][8] = {{0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}};
    const int group = rate >> 2, select = rate & 3;
    if (group >= 12) return std::max(1, ((4 + select) << (group - 12)) >> 2);
    const int shift = 11 - group;
    if (counter & ((1u << shift) - 1)) return 0;
    return kPattern[select][(counter >> shift) & 7];
}

}  // namespace

uint32_t Scsp::read_ram(uint32_t offset, int bytes) const {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | ram_[(offset + i) & kRamMask];
    return value;
}

void Scsp::write_ram(uint32_t offset, int bytes, uint32_t value) {
    for (int i = 0; i < bytes; ++i) ram_[(offset + i) & kRamMask] = (value >> (8 * (bytes - 1 - i))) & 0xFF;
}

uint32_t Scsp::read_reg(uint32_t offset, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | ((offset + i) < kRegisterSize ? read_register_byte(offset + i) : 0);
    return value;
}

void Scsp::write_reg(uint32_t offset, int bytes, uint32_t value) {
    for (int i = 0; i < bytes; ++i)
        if (offset + i < kRegisterSize) write_register_byte(offset + i, (value >> (8 * (bytes - 1 - i))) & 0xFF);
}

bool Scsp::dsp_work_byte(uint32_t offset, int32_t*& word, int& shift, uint32_t& mask) {
    int bits;
    if (offset < kDspMems) { word = &dsp_.temp[(offset - kDspTemp) >> 2]; bits = 24; }
    else if (offset < kDspMixs) { word = &dsp_.mems[(offset - kDspMems) >> 2]; bits = 24; }
    else { word = &dsp_.mixs[(offset - kDspMixs) >> 2]; bits = 20; }
    const int low = bits - 16;   // bits guardados na primeira palavra (a segunda guarda os 16 de cima)
    switch (offset & 3) {
        case 1: shift = 0; mask = (1u << low) - 1; return true;
        case 2: shift = bits - 8; mask = 0xFF; return true;
        case 3: shift = low; mask = 0xFF; return true;
        default: return false;   // byte alto da primeira palavra: sem bits
    }
}

uint8_t Scsp::read_register_byte(uint32_t offset) {
    int32_t* word;
    int shift;
    uint32_t mask;
    if (offset >= kDspTemp && offset < kDspEfreg) return dsp_work_byte(offset, word, shift, mask) ? (uint32_t(*word) >> shift) & mask : 0;
    return regs_[offset];
}

// Escrita de um byte. Registradores de interrupção têm semântica própria; o byte escrito vale como se o outro byte da palavra fosse 0.
void Scsp::write_register_byte(uint32_t offset, uint8_t value) {
    if (offset >= kDspTemp && offset < kDspEfreg) {   // trabalho do DSP: estado interno, não o arquivo de bytes
        int32_t* word;
        int shift;
        uint32_t mask;
        if (dsp_work_byte(offset, word, shift, mask)) *word = int32_t((uint32_t(*word) & ~(mask << shift)) | ((value & mask) << shift));
        return;
    }
    if (offset >= kDspExts) return;   // EXTS é só de leitura
    if (offset >= kDspProgram && offset < kDspTemp && value) dsp_active_ = true;
    const uint32_t reg = offset & ~1u;
    const uint16_t written = (offset & 1) ? value : uint16_t(value << 8);
    if (reg < kSlotCount * kSlotStride && !(reg % kSlotStride)) {   // controle do slot: KYONEX dispara o liga/desliga de todos
        regs_[offset] = value;
        if (written & kKeyOnExecute) key_on_off();
        return;
    }
    switch (reg) {
        case kScipd:   // só o bit 5 (interrupção manual do SH-2 para o 68000) é gravável
            if (written & kManual) set_word(kScipd, word(kScipd) | kManual);
            break;
        case kMcipd:   // idem, no sentido do 68000 para o SH-2
            if (written & kManual) set_word(kMcipd, word(kMcipd) | kManual);
            break;
        case kScire: set_word(kScipd, word(kScipd) & ~written); break;   // escrever 1 limpa a pendência
        case kMcire: set_word(kMcipd, word(kMcipd) & ~written); break;
        case kMonitor: regs_[offset] = value; update_monitor(); return;
        default:
            regs_[offset] = value;
            if (!is_interrupt_register(reg)) return;
    }
    refresh_interrupts();
}

// Pendência levantada pelo hardware (timers, amostra) vale para os dois lados; a interrupção manual do SH-2 só para o 68000.
void Scsp::set_pending(uint16_t bits) {
    set_word(kScipd, word(kScipd) | bits);
    set_word(kMcipd, word(kMcipd) | bits);
    refresh_interrupts();
}

int Scsp::cpu_level() const {
    const uint16_t active = word(kScipd) & word(kScieb) & kPendingMask;
    int level = 0;
    for (int source = 0; source < 11; ++source) {
        if (!(active & (1 << source))) continue;
        const int source_level = ((word(kScilv0) >> source) & 1) | (((word(kScilv1) >> source) & 1) << 1) | (((word(kScilv2) >> source) & 1) << 2);
        if (source_level > level) level = source_level;
    }
    return level;
}

// Sinaliza só as pendências habilitadas que acabaram de surgir (as interrupções do SCSP são tratadas como borda).
void Scsp::refresh_interrupts() {
    const uint16_t cpu_now = word(kScipd) & word(kScieb) & kPendingMask;
    const bool cpu_new = cpu_now & ~active_cpu_;
    active_cpu_ = cpu_now;
    if (cpu_new && running_) {
        if (const int level = cpu_level()) cpu_.raise_interrupt(level);
    }
    const uint16_t main_now = word(kMcipd) & word(kMcieb) & kPendingMask;
    const bool main_new = main_now & ~active_main_;
    active_main_ = main_now;
    if (main_new && on_main_interrupt) on_main_interrupt();
}

void Scsp::tick_timer(Reg timer, uint16_t bit) {
    const int shift = (word(timer) >> kTimerPrescalerShift) & kTimerPrescalerMask;
    if (samples_ & ((1u << shift) - 1)) return;
    if (++regs_[timer + 1] == 0) set_pending(bit);
}

// KYONEX: cada slot recebe KEY ON (reinicia posição e envelope) ou KEY OFF (release) conforme o seu KYONB.
void Scsp::key_on_off() {
    for (int i = 0; i < kSlotCount; ++i) {
        const uint32_t control = i * kSlotStride;
        const bool key_on = word(control) & kKeyOnBit;
        Slot& slot = slots_[i];
        if (key_on && !slot.key) {
            slot.position = 0;
            slot.reversed = false;
            slot.playing = true;
            slot.eg = Eg::kAttack;
            slot.attenuation = 0x3FF;
        } else if (!key_on && slot.key) {
            slot.eg = Eg::kRelease;
        }
        slot.key = key_on;
        set_word(control, word(control) & ~kKeyOnExecute);
    }
}

// Avança a posição de cada slot ativo conforme a altura e o laço (LPCTL: 0 sem laço, 1 normal, 2 reverso, 3 alternado).
void Scsp::advance_slots() {
    for (int i = 0; i < kSlotCount; ++i) {
        Slot& slot = slots_[i];
        if (!slot.playing) continue;
        const uint32_t base = i * kSlotStride;
        const uint16_t pitch = word(base + kSlotPitch);
        const int64_t step = int64_t(1024 + (pitch & 0x3FF)) << (kPositionFractionBits - 10 + slot_octave(pitch));
        const int64_t end = int64_t(word(base + kSlotLoopEnd)) << kPositionFractionBits;
        const int64_t start = int64_t(word(base + kSlotLoopStart)) << kPositionFractionBits;
        const int mode = (word(base) >> 5) & 3;
        if (slot.reversed) {
            slot.position -= step;
            if (slot.position >= start) continue;
            if (mode == 3) { slot.position = 2 * start - slot.position; slot.reversed = false; }
            else slot.position += end - start;   // reverso: volta ao fim do laço, ainda lendo para trás
            continue;
        }
        slot.position += step;
        if (slot.position < end) continue;
        if (!mode) { slot.playing = false; slot.position = end; continue; }
        if (end <= start) { slot.position = start; continue; }
        if (mode == 1) slot.position -= end - start;
        else { slot.position = 2 * end - slot.position; slot.reversed = true; }
    }
}

// CA (bits 10–7 do registrador de monitoração): endereço de leitura >> 12 do slot escolhido em MSLC.
void Scsp::update_monitor() {
    const uint16_t monitor = word(kMonitor);
    const uint64_t address = uint64_t(slots_[(monitor >> kMonitorSlotShift) & (kSlotCount - 1)].position) >> kPositionFractionBits;
    set_word(kMonitor, (monitor & 0xF800) | ((address >> kCallAddressShift) & 0xF) << kMonitorAddressShift);
}

void Scsp::sample_tick() {
    ++samples_;
    advance_slots();
    update_monitor();
    // O envelope é estado do hardware (o fim do release desliga o slot): avança sempre; só a mixagem depende de haver saída.
    ++eg_counter_;
    for (int i = 0; i < kSlotCount; ++i)
        if (slots_[i].playing) step_envelope(i);
    if (audio_enabled_) mix_audio();
    tick_timer(kTimerA, kTimerABit);
    tick_timer(kTimerB, kTimerBBit);
    tick_timer(kTimerC, kTimerCBit);
    set_pending(kSample);
}

void Scsp::power(bool on) {
    running_ = on;
    cpu_credit_ = 0;
    if (!on) return;
    cpu_.reset();
    active_cpu_ = 0;
    refresh_interrupts();
}

void Scsp::advance(uint32_t master_cycles, uint32_t master_hz) {
    sample_credit_ += int64_t(master_cycles) * kSampleHz;
    while (sample_credit_ >= int64_t(master_hz)) {
        sample_credit_ -= master_hz;
        sample_tick();
    }
    if (!running_) return;
    cpu_credit_ += int64_t(master_cycles) * kM68kHz;
    while (cpu_credit_ > 0) cpu_credit_ -= int64_t(cpu_.step()) * master_hz;
}

uint32_t Scsp::read68k(uint32_t addr, int bytes) {
    addr &= kM68kAddressMask;
    const uint32_t value = addr < kM68kRegisterBase ? read_ram(addr, bytes) : read_reg(addr & kRegisterMask, bytes);
    if (on_68k_access) on_68k_access(addr, bytes, value, false);
    return value;
}

void Scsp::write68k(uint32_t addr, int bytes, uint32_t value) {
    addr &= kM68kAddressMask;
    if (on_68k_access) on_68k_access(addr, bytes, value, true);
    if (addr < kM68kRegisterBase) write_ram(addr, bytes, value);
    else write_reg(addr & kRegisterMask, bytes, value);
}


void Scsp::save(saturn::StateWriter& w) const {
    w.raw(ram_);
    w.raw(regs_);
    cpu_.save(w);
    w.boolean(running_);
    w.u64(samples_);
    w.i64(sample_credit_);
    w.i64(cpu_credit_);
    for (const Slot& slot : slots_) {
        w.boolean(slot.playing);
        w.boolean(slot.key);
        w.boolean(slot.reversed);
        w.u8(static_cast<uint8_t>(slot.eg));
        w.u16(slot.attenuation);
        w.i64(slot.position);
    }
    w.u32(eg_counter_);
    w.u32(noise_);
    w.i16(ext_left_);
    w.i16(ext_right_);
    w.u16(active_cpu_);
    w.u16(active_main_);
    for (int32_t v : dsp_.temp) w.i32(v);
    for (int32_t v : dsp_.mems) w.i32(v);
    for (int32_t v : dsp_.mixs) w.i32(v);
    w.i32(dsp_.inputs);
    w.i32(dsp_.y_reg);
    w.u32(dsp_.sft_reg);
    w.u16(dsp_.frc_reg);
    w.u16(dsp_.adrs_reg);
    w.u16(dsp_.mdec_ct);
    w.u32(dsp_.rw_addr);
    w.u32(dsp_.read_value);
    w.u16(dsp_.write_value);
    w.u8(dsp_.read_pending);
    w.boolean(dsp_.write_pending);
}

void Scsp::load(saturn::StateReader& r) {
    r.raw(ram_);
    r.raw(regs_);
    cpu_.load(r);
    running_ = r.boolean();
    samples_ = r.u64();
    sample_credit_ = r.i64();
    cpu_credit_ = r.i64();
    for (Slot& slot : slots_) {
        slot.playing = r.boolean();
        slot.key = r.boolean();
        slot.reversed = r.boolean();
        const uint8_t eg = r.u8();
        if (eg > static_cast<uint8_t>(Eg::kRelease)) throw saturn::StateError("save state corrompido: fase de envelope inválida");
        slot.eg = static_cast<Eg>(eg);
        slot.attenuation = r.u16();
        slot.position = r.i64();
    }
    eg_counter_ = r.u32();
    noise_ = r.u32();
    ext_left_ = r.i16();
    ext_right_ = r.i16();
    active_cpu_ = r.u16();
    active_main_ = r.u16();
    for (int32_t& v : dsp_.temp) v = r.i32();
    for (int32_t& v : dsp_.mems) v = r.i32();
    for (int32_t& v : dsp_.mixs) v = r.i32();
    dsp_.inputs = r.i32();
    dsp_.y_reg = r.i32();
    dsp_.sft_reg = r.u32();
    dsp_.frc_reg = r.u16();
    dsp_.adrs_reg = r.u16();
    dsp_.mdec_ct = r.u16();
    dsp_.rw_addr = r.u32();
    dsp_.read_value = r.u32();
    dsp_.write_value = r.u16();
    dsp_.read_pending = r.u8();
    if (dsp_.read_pending > 2) throw saturn::StateError("save state corrompido: leitura pendente do DSP inválida");
    dsp_.write_pending = r.boolean();
    dsp_active_ = std::any_of(regs_.begin() + kDspProgram, regs_.begin() + kDspTemp, [](uint8_t b) { return b != 0; });
    audio_read_ = audio_write_ = 0;   // o anel de saída é descartado
}

void Scsp::set_audio_enabled(bool enabled) {
    audio_enabled_ = enabled;
    if (enabled && audio_.empty()) audio_.assign(kAudioBufferFrames * 2, 0);
    if (!enabled) audio_read_ = audio_write_ = 0;
}

size_t Scsp::drain_audio(std::span<int16_t> out) {
    const size_t frames = std::min<size_t>(out.size() / 2, audio_write_ - audio_read_);
    for (size_t i = 0; i < frames; ++i) {
        const size_t at = (audio_read_++ & kAudioRingMask) * 2;
        out[2 * i] = audio_[at];
        out[2 * i + 1] = audio_[at + 1];
    }
    return frames * 2;
}

// Uma amostra de 16 bits do slot; PCM8B (bit 4) e SBCTL (bits 10-9) vêm da palavra de controle `control`.
int16_t Scsp::fetch_pcm(uint32_t base_address, int control, uint32_t index) const {
    uint16_t raw;
    if (control & 0x10) raw = uint16_t(ram_[(base_address + index) & kRamMask] << 8);   // PCM8B: 8 bits com sinal
    else raw = uint16_t((ram_[(base_address + 2 * index) & kRamMask] << 8) | ram_[(base_address + 2 * index + 1) & kRamMask]);
    const int sbctl = (control >> 9) & 3;   // SBCTL: bit 0 inverte o bit de sinal, bit 1 inverte os demais
    if (sbctl & 1) raw ^= 0x8000;
    if (sbctl & 2) raw ^= 0x7FFF;
    return int16_t(raw);
}

// Avança o envelope ADSR de um slot em uma amostra. Taxa efetiva = 2*taxa + deslocamento de KRS/oitava (KRS = 15 desliga o deslocamento).
void Scsp::step_envelope(int index) {
    Slot& slot = slots_[index];
    const uint32_t base = index * kSlotStride;
    const uint16_t rates = word(base + kSlotRates), release = word(base + kSlotRelease);
    const int krs = (release >> 10) & 0xF;
    const int offset = krs == 0xF ? 0 : std::clamp(krs + slot_octave(word(base + kSlotPitch)), 0, 15);
    const auto effective = [&](int raw) { return raw ? std::clamp(2 * raw + offset, 0, 63) : -1; };
    switch (slot.eg) {
        case Eg::kAttack: {
            const int raw = rates & 0x1F;
            if (raw == 0x1F) slot.attenuation = 0;
            else if (const int rate = effective(raw); rate >= 0 && slot.attenuation > 0) {
                const int inc = envelope_increment(rate, eg_counter_);
                if (inc) slot.attenuation -= std::min<int>(slot.attenuation, ((slot.attenuation * inc) >> 4) + 1);
            }
            if (slot.attenuation == 0) slot.eg = Eg::kDecay1;
            break;
        }
        case Eg::kDecay1:
        case Eg::kDecay2: {
            const int rate = effective(slot.eg == Eg::kDecay1 ? (rates >> 6) & 0x1F : (rates >> 11) & 0x1F);
            if (rate >= 0) slot.attenuation = std::min(0x3FF, slot.attenuation + envelope_increment(rate, eg_counter_));
            if (slot.eg == Eg::kDecay1 && slot.attenuation >= ((release >> 5) & 0x1F) * 32) slot.eg = Eg::kDecay2;
            break;
        }
        case Eg::kRelease: {
            const int rate = effective(release & 0x1F);
            if (rate >= 0) slot.attenuation = std::min(0x3FF, slot.attenuation + envelope_increment(rate, eg_counter_));
            if (slot.attenuation >= 0x3FF) slot.playing = false;
            break;
        }
    }
}

namespace {

int32_t sign_extend(uint32_t value, int bits) { return int32_t(value << (32 - bits)) >> (32 - bits); }

// Ponto flutuante de 16 bits do DSP (sinal, expoente de 4 bits, mantissa de 11) <-> inteiro de 24 bits com sinal.
uint32_t dsp_float_to_int(uint16_t value) {
    const int exponent = (value >> 11) & 0xF;
    uint32_t mantissa = value & 0x7FF;
    if (exponent < 12) mantissa |= 0x800;   // bit implícito
    mantissa <<= 19;
    if (value & 0x8000) mantissa ^= 0xC0000000u;   // negativo: inverte os dois bits altos (sinal e bit implícito)
    return uint32_t(int32_t(mantissa) >> (8 + std::min(11, exponent))) & 0xFFFFFF;
}

uint16_t dsp_int_to_float(uint32_t value) {
    const int32_t shifted = int32_t(value << 8);   // 24 bits no topo; o sinal decide a normalização
    const uint32_t magnitude = uint32_t(shifted ^ (shifted >> 31));
    const int exponent = std::countl_zero((magnitude << 1) | (1u << 19));
    const int shift = exponent == 12 ? 11 : exponent;
    return uint16_t(((shifted >> (19 - shift)) & 0x87FF) | (exponent << 11));
}

}  // namespace

// Um passo do DSP por amostra: 128 instruções de 64 bits (MPRO) sobre as entradas (MEMS/MIXS/EXTS), os coeficientes (COEF), o TEMP e
// o anel de atraso na RAM de som (MADRS, RBP/RBL). A leitura/escrita de memória pedida num passo se completa no seguinte.
void Scsp::run_dsp() {
    const uint16_t ring = word(kRingBuffer);
    const unsigned ring_length = ring >> 7 & 3, ring_base = ring & 0x7F;
    Dsp& d = dsp_;
    for (int step = 0; step < kDspSteps; ++step) {
        uint64_t instr = 0;
        for (int i = 0; i < 8; ++i) instr = (instr << 8) | regs_[kDspProgram + 8 * step + i];
        const bool nxaddr = instr & 1, adrgb = instr >> 1 & 1, nofl = instr >> 8 & 1, bsel = instr >> 16 & 1, zero = instr >> 17 & 1,
                   negb = instr >> 18 & 1, yrl = instr >> 19 & 1, shft0 = instr >> 20 & 1, shft1 = instr >> 21 & 1, frcl = instr >> 22 & 1,
                   adrl = instr >> 23 & 1, ewt = instr >> 28 & 1, mrt = instr >> 29 & 1, mwt = instr >> 30 & 1, table = instr >> 31 & 1,
                   iwt = instr >> 37 & 1, xsel = instr >> 47 & 1, twt = instr >> 55 & 1;
        const unsigned masa = instr >> 2 & 0x1F, cra = instr >> 9 & 0x3F, ewa = instr >> 24 & 0xF, iwa = instr >> 32 & 0x1F,
                       ira = instr >> 38 & 0x3F, ysel = instr >> 45 & 3;
        const unsigned temp_write = ((instr >> 48) + d.mdec_ct) & 0x7F, temp_read = ((instr >> 56) + d.mdec_ct) & 0x7F;

        // Entrada: 0x00–0x1F MEMS, 0x20–0x2F MIXS, 0x30–0x31 EXTS (as demais mantêm o valor anterior).
        if (!(ira & 0x20)) d.inputs = d.mems[ira & 0x1F];
        else if (!(ira & 0x10)) d.inputs = (d.mixs[ira & 0xF] << 4) & 0xFFFFFF;
        else if (!(ira & 0xE)) d.inputs = (int32_t(int16_t(ira & 1 ? ext_right_ : ext_left_)) << 8) & 0xFFFFFF;
        const int32_t inputs = sign_extend(d.inputs, 24);
        const uint16_t coefficient = word(kDspCoef + 2 * cra) >> 3;
        const uint32_t y_inputs[4] = {d.frc_reg, coefficient, uint32_t(d.y_reg >> 11) & 0x1FFF, uint32_t(d.y_reg >> 4) & 0xFFF};
        if (yrl) d.y_reg = inputs & 0xFFFFFF;

        // Saída do deslocador (saturada em 24 bits quando SHFT1 = 0).
        int64_t shifted = int64_t(sign_extend(d.sft_reg, 26)) << (shft0 ^ shft1);
        if (!shft1) shifted = std::clamp<int64_t>(shifted, -0x800000, 0x7FFFFF);
        const uint32_t shifter = uint32_t(shifted) & 0xFFFFFF;
        if (frcl) d.frc_reg = (shft0 && shft1) ? shifter & 0xFFF : shifter >> 11;

        // Multiplicador/somador.
        const int32_t temp = sign_extend(d.temp[temp_read], 24);
        const int64_t product = (int64_t(sign_extend(y_inputs[ysel], 13)) * (xsel ? inputs : temp)) >> 12;
        uint32_t addend = bsel ? d.sft_reg : uint32_t(temp);
        if (negb) addend = 0u - addend;
        if (zero) addend = 0;
        d.sft_reg = (uint32_t(product) + addend) & 0x3FFFFFF;

        if (ewt) set_word(kDspEfreg + 2 * ewa, uint16_t(shifter >> 8));
        if (twt) d.temp[temp_write] = int32_t(shifter);
        if (iwt) d.mems[iwa] = int32_t(d.read_value);

        // Conclui o acesso à RAM pedido no passo anterior.
        const uint32_t byte_address = (d.rw_addr << 1) & kRamMask;
        if (d.read_pending) {
            const uint16_t raw = uint16_t((ram_[byte_address] << 8) | ram_[byte_address + 1]);
            d.read_value = d.read_pending == 2 ? uint32_t(raw) << 8 : dsp_float_to_int(raw);
            d.read_pending = 0;
        } else if (d.write_pending) {
            if (!(d.rw_addr & 0x40000)) {
                ram_[byte_address] = d.write_value >> 8;
                ram_[byte_address + 1] = d.write_value & 0xFF;
            }
            d.write_pending = false;
        }

        // Endereço do próximo acesso: MADRS + NXADDR (+ ADRS_REG) (+ posição do anel, exceto em tabelas).
        uint16_t address = uint16_t(word(kDspMadrs + 2 * masa) + nxaddr);
        if (adrgb) address += sign_extend(d.adrs_reg, 12);
        if (!table) address = (address + d.mdec_ct) & ((0x2000u << ring_length) - 1);
        d.rw_addr = (address + (ring_base << 12)) & 0x7FFFF;
        if (mrt) d.read_pending = nofl ? 2 : 1;
        if (mwt) {
            d.write_pending = true;
            d.write_value = nofl ? uint16_t(shifter >> 8) : dsp_int_to_float(shifter);
        }
        if (adrl) d.adrs_reg = (shft0 && shft1) ? uint16_t(shifter >> 12) : uint16_t((inputs >> 16) & 0xFFF);
    }
    if (!d.mdec_ct) d.mdec_ct = uint16_t(0x2000u << ring_length);
    --d.mdec_ct;
}

// Gera um quadro estéreo: roda o DSP, soma a saída direta dos slots (DISDL/DIPAN) e a de efeitos (EFSDL/EFPAN: EFREG nos slots 0–15,
// EXTS nos 16–17), envia os slots ao DSP (ISEL/IMXL) e aplica MVOL com saturação.
void Scsp::mix_audio() {
    if (dsp_active_) run_dsp();
    std::fill(std::begin(dsp_.mixs), std::end(dsp_.mixs), 0);
    int32_t left = 0, right = 0;
    // Nível (já atenuado por DISDL/EFSDL) com o pan de 5 bits: o bit 4 escolhe o canal atenuado (1 = direito, 0 = esquerdo, como no
    // Mednafen; a gravação de referência do BIOS coincide com ρ = 1,000); ele perde 3 dB por passo e o outro fica inteiro.
    const auto add_panned = [&](int32_t level, int pan) {
        const uint32_t attenuated = kGain.pan[pan & 0xF];
        const bool attenuate_right = pan & 0x10;
        const uint32_t gain_left = attenuate_right ? 65536u : attenuated, gain_right = attenuate_right ? attenuated : 65536u;
        left += int32_t((int64_t(level) * gain_left) >> 16);
        right += int32_t((int64_t(level) * gain_right) >> 16);
    };
    for (int i = 0; i < kSlotCount; ++i) {
        const uint32_t base = i * kSlotStride;
        const uint16_t out = word(base + kSlotOutput);
        const int disdl = out >> 13, efsdl = (out >> 5) & 7;
        const int to_dsp = word(base + kSlotInput) & 7;
        Slot& slot = slots_[i];
        if (slot.playing && (disdl || to_dsp)) {
            const uint16_t w0 = word(base), tl_word = word(base + kSlotLevel);
            int32_t sample = 0;
            const int source = (w0 >> 7) & 3;   // SSCTL
            if (source == 1) {   // ruído: LFSR de 16 bits
                noise_ = (noise_ >> 1) | (((noise_ ^ (noise_ >> 1)) & 1) << 15);
                sample = int16_t(noise_);
            } else if (!source) {   // 2 e 3: silêncio
                const uint32_t sa = ((w0 & 0xF) << 16) | word(base + kSlotStartLow);
                const uint32_t index = uint32_t(slot.position >> kPositionFractionBits);
                const int32_t s0 = fetch_pcm(sa, w0, index), s1 = fetch_pcm(sa, w0, index + 1);
                const int32_t fraction = int32_t((slot.position >> (kPositionFractionBits - 16)) & 0xFFFF);
                sample = s0 + (((s1 - s0) * fraction) >> 16);
            }
            // SDIR (bit 8) ignora o envelope e o TL.
            const int attenuation = (tl_word & 0x100) ? 0 : std::min(0x3FF, slot.attenuation + (tl_word & 0xFF) * 4);
            const int32_t shaped = int32_t((int64_t(sample) * kGain.envelope[attenuation]) >> 16);
            if (disdl) add_panned(shaped >> (7 - disdl), (out >> 8) & 0x1F);
            if (to_dsp) {   // ISEL escolhe a entrada MIXS; o nível IMXL atenua 6 dB por passo (o MIXS tem 4 bits fracionários a mais)
                int32_t& mix = dsp_.mixs[(word(base + kSlotInput) >> 3) & 0xF];
                mix = (mix + ((shaped * 16) >> (7 - to_dsp))) & 0xFFFFF;
            }
        }
        if (efsdl) {
            const int16_t effect = i < 16 ? int16_t(word(kDspEfreg + 2 * i)) : i == 16 ? ext_left_ : i == 17 ? ext_right_ : int16_t(0);
            add_panned(int32_t(effect) >> (7 - efsdl), out & 0x1F);
        }
    }
    const uint32_t master = kGain.master[word(kMasterVolume) & 0xF];
    const auto finish = [&](int32_t v) { return int16_t(std::clamp<int64_t>((int64_t(v) * master) >> 16, -32768, 32767)); };
    // Anel com contadores livres de leitura/escrita (tamanho potência de 2): cheio, descarta o quadro mais antigo.
    if (audio_write_ - audio_read_ == kAudioBufferFrames) ++audio_read_;
    const size_t at = (audio_write_++ & kAudioRingMask) * 2;
    audio_[at] = finish(left);
    audio_[at + 1] = finish(right);
}

}  // namespace audio
