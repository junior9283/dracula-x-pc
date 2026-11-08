// Laços ociosos do SH-2: detecção, gravação de uma volta e conferência (ver Cpu::set_idle_skip em cpu.hpp).
#include <cstring>

#include "sh2/cpu.hpp"

namespace sh2 {

void Cpu::set_idle_skip(bool on) {
    if (!on) {
        leave_idle();
        abort_record();
    }
    idle_enabled_ = on && !recorder_;
}

void Cpu::leave_idle() {
    if (!idle_on_) return;
    idle_on_ = false;
    s_ = loop_.state[idle_phase_];
    op_address_ = loop_.op_address[idle_phase_];
}

// Uma interrupção pendente só interrompe o laço se for atendível; a máscara não muda dentro do laço (conferido na gravação).
bool Cpu::idle_irq_due() const {
    const int mask = (loop_.state[0].sr & kSrIMask) >> 4;
    return pending_level_ == kNmiLevel || pending_level_ > mask;
}

bool Cpu::idle_revalidate() {
    if (bus_.map_generation() != loop_gen_) return false;
    for (int i = 0; i < loop_.watch_count; ++i) {
        const IdleLoop::Watch& w = loop_.watches[i];
        uint32_t word;
        std::memcpy(&word, w.word, sizeof word);
        if ((word & w.mask) != w.value) return false;
    }
    for (int i = 0; i < loop_.byte_watch_count; ++i)
        if (*loop_.byte_watches[i].byte != loop_.byte_watches[i].value) return false;
    idle_epoch_ = bus_.epoch();
    return true;
}

void Cpu::begin_record() {
    cand_hits_ = 0;
    sample_count_ = 0;
    for (uint32_t head : rejected_heads_)
        if (head == s_.pc) return;
    recording_ = true;
    fast_areas_ = 0;   // todo acesso à memória passa por read_slow/write_slow, que vigiam e rejeitam o que não é leitura de RAM/ROM
    record_steps_ = 0;
    record_start_ = s_;
    record_start_op_ = op_address_;
    loop_.watch_count = 0;
    loop_.byte_watch_count = 0;
    loop_gen_ = bus_.map_generation();
}

void Cpu::fail_record() {
    abort_record();
    rejected_heads_[rejected_next_++ % 8] = record_start_.pc;
}

// Vigia os bytes [address, address + bytes) de RAM/ROM: a palavra alinhada que os contém deve manter esses bytes.
bool Cpu::add_watch(uint32_t address, int bytes) {
    const uint32_t in_page = address & saturn::Bus::kPageMask;
    if ((address >> 29) > 1 || (address & (bytes - 1)) != 0) return false;
    const uint8_t* page = read_pages_[(address & 0x1FFFFFFF) >> saturn::Bus::kPageShift];
    if (!page || in_page + bytes > saturn::Bus::kPageSize) return false;
    const uint8_t* p = page + in_page;
    const auto offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) & 3);
    const uint8_t* word = p - offset;
    const uint32_t mask = (bytes == 4 ? 0xFFFFFFFFu : ((1u << (8 * bytes)) - 1)) << (8 * offset);
    uint32_t value;
    std::memcpy(&value, word, sizeof value);
    value &= mask;
    for (int i = 0; i < loop_.watch_count; ++i) {
        IdleLoop::Watch& w = loop_.watches[i];
        if (w.word != word) continue;
        w.mask |= mask;
        w.value = (w.value & ~mask) | value;
        return true;
    }
    if (loop_.watch_count == IdleLoop::kMaxWatches) return false;
    loop_.watches[loop_.watch_count++] = {word, mask, value};
    return true;
}

bool Cpu::watch_read(uint32_t address, int bytes) {
    if (address < OnChip::kBase) return add_watch(address, bytes);
    for (int i = 0; i < bytes; ++i) {
        const uint8_t* byte = onchip_.stable_byte(address - OnChip::kBase + i);
        if (!byte || loop_.byte_watch_count == IdleLoop::kMaxWatches) return false;
        loop_.byte_watches[loop_.byte_watch_count++] = {byte, *byte};
    }
    return true;
}

// Chamado depois de cada passo executado durante a gravação.
void Cpu::record_step(uint32_t cycles) {
    const int n = record_steps_;
    // Passo com efeito além de registradores e leituras (LDC SR, SLEEP) ou código fora de RAM/ROM: não é laço ocioso.
    if (n == IdleLoop::kMaxSteps || inhibit_irq_ || sleeping_ || (s_.sr & kSrIMask) != (record_start_.sr & kSrIMask)) {
        fail_record();
        return;
    }
    // O código (a instrução e o possível delay slot) também tem de continuar igual.
    if (!add_watch(op_address_, 2) || !add_watch(op_address_ + 2, 2)) {
        fail_record();
        return;
    }
    loop_.cycles[n] = cycles;
    record_state_[n] = s_;
    record_op_[n] = op_address_;
    record_steps_ = n + 1;
    if (s_.pc != record_start_.pc) return;

    // Voltou ao início: só é laço ocioso se os registradores (e o último op_address_) voltaram idênticos.
    if (s_ != record_start_ || op_address_ != record_start_op_ || loop_gen_ != bus_.map_generation()) {
        fail_record();
        return;
    }
    finish_record_ok();
}

void Cpu::finish_record_ok() {
    const int n = record_steps_;
    // Fase p = estado depois de p passos da volta; a fase 0 é o estado atual (fim da volta gravada).
    loop_.steps = n;
    for (int p = 0; p < n; ++p) {
        const int src = (p + n - 1) % n;   // estado depois do passo p-1
        loop_.state[p] = record_state_[src];
        loop_.op_address[p] = record_op_[src];
        if (loop_.state[p].pc >= idle_excl_begin_ && loop_.state[p].pc < idle_excl_end_) { fail_record(); return; }
    }
    abort_record();   // encerra a gravação (restaura o caminho rápido)
    idle_on_ = true;
    idle_phase_ = 0;
    idle_epoch_ = bus_.epoch();
}

}  // namespace sh2
