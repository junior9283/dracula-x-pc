// SCSP (Saturn Custom Sound Processor): RAM de som, arquivo de registradores, controlador de interrupções, timers A/B/C, o 68000 que
// roda o driver de som e a síntese dos 32 slots (PCM 8/16 bits, laços, envelope ADSR, TL, saída direta DISDL/DIPAN, MVOL) em estéreo
// a 44,1 kHz, o envio aos efeitos (ISEL/IMXL), o DSP de efeitos (MPRO/COEF/MADRS, anel de atraso na RAM de som) e a saída dos efeitos
// (EFSDL/EFPAN, EFREG e entradas externas EXTS). Pendências: FM (MDL/MDXSL/MDYSL) e LFO.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "audio/m68k.hpp"
#include "saturn/state.hpp"

namespace audio {

class Scsp : public M68k::Bus {
public:
    static constexpr uint32_t kRamSize = 0x80000, kRegisterSize = 0x1000;
    // Registradores comuns (deslocamento no arquivo de registradores, 16 bits cada).
    enum Reg : uint32_t {
        kTimerA = 0x418, kTimerB = 0x41A, kTimerC = 0x41C,
        kScieb = 0x41E, kScipd = 0x420, kScire = 0x422, kScilv0 = 0x424, kScilv1 = 0x426, kScilv2 = 0x428,
        kMcieb = 0x42A, kMcipd = 0x42C, kMcire = 0x42E,
        kRingBuffer = 0x402,   // RBL (bits 8–7, tamanho do anel do DSP) e RBP (bits 6–0, base em blocos de 8 KiB)
        kMonitor = 0x408,   // MSLC (bits 15–11, slot monitorado) e CA (bits 10–7, endereço de leitura >> 12 do slot monitorado)
    };
    // Área do DSP no arquivo de registradores.
    static constexpr uint32_t kDspCoef = 0x700, kDspMadrs = 0x780, kDspProgram = 0x800, kDspTemp = 0xC00, kDspMems = 0xE00,
                              kDspMixs = 0xE80, kDspEfreg = 0xEC0, kDspExts = 0xEE0;
    static constexpr int kSlotCount = 32, kDspSteps = 128;
    // Bits de SCIPD/MCIPD.
    enum Pending : uint16_t {
        kDmaEnd = 1 << 4, kManual = 1 << 5, kTimerABit = 1 << 6, kTimerBBit = 1 << 7, kTimerCBit = 1 << 8, kSample = 1 << 10,
        kPendingMask = 0x7FF,
    };
    static constexpr uint64_t kSampleHz = 44'100, kM68kHz = 11'289'600;


    Scsp() { ram_.assign(kRamSize, 0); regs_.assign(kRegisterSize, 0); }

    // RAM de som vista pelo SH-2 (base 0x25A00000; espelhada a cada 512 KiB) e registradores (base 0x25B00000).
    uint32_t read_ram(uint32_t offset, int bytes) const;
    void write_ram(uint32_t offset, int bytes, uint32_t value);
    uint32_t read_reg(uint32_t offset, int bytes);
    void write_reg(uint32_t offset, int bytes, uint32_t value);
    std::span<uint8_t> ram() { return ram_; }

    // Saída de áudio. Desligada por padrão (sem custo para quem não consome). Ligada, cada amostra (44,1 kHz) gera um quadro estéreo.
    void set_audio_enabled(bool enabled);
    // Copia até out.size()/2 quadros intercalados (L,R) e os remove do buffer interno; devolve quantos int16 foram copiados.
    size_t drain_audio(std::span<int16_t> out);
    // Entrada externa (EXTS, p.ex. CD-DA), válida até ser trocada. Como no hardware, entra pelas saídas de efeito dos slots 16 e 17 (EFSDL/EFPAN) e pelo DSP (EXTS), não direto na mixagem.
    void set_external_input(int16_t left, int16_t right) {
        ext_left_ = left;
        ext_right_ = right;
        set_word(kDspExts, uint16_t(left));
        set_word(kDspExts + 2, uint16_t(right));
    }
    static constexpr size_t kAudioBufferFrames = 1 << 16;   // ~1,5 s; ao encher, o mais antigo é descartado

    // O 68000 só executa depois de ligado (SMPC SNDON). Ao ligar, reseta lendo SSP/PC dos 8 primeiros bytes da RAM.
    void power(bool on);
    bool running() const { return running_; }
    M68k& cpu() { return cpu_; }
    // Avança o hardware `master_cycles` ciclos do SH-2 mestre (28,63636 MHz): timers, amostras e o 68000.
    // Avança `master_cycles` ciclos de quem chama, cujo relógio é `master_hz` (a CPU do Saturn: 26,85 ou 28,64 MHz conforme o
    // modo de vídeo); o SCSP tem relógio próprio (44,1 kHz) e converte por ele.
    void advance(uint32_t master_cycles, uint32_t master_hz);

    // Save state: RAM, registradores, 68000, slots (posição/envelope/laço), timers, créditos, ruído e entrada externa. O anel de saída
    // de áudio é descartado no load (quem consome recomeça do zero); a ligação do áudio e os callbacks não entram.
    void save(saturn::StateWriter& w) const;
    void load(saturn::StateReader& r);

    // Interrupção para o SH-2 (SCU SoundRequest): chamada quando surge uma pendência habilitada em MCIPD.
    std::function<void()> on_main_interrupt;
    // Diagnóstico opcional: acessos do 68000 (endereço, bytes, valor lido/escrito, true = escrita).
    std::function<void(uint32_t address, int bytes, uint32_t value, bool write)> on_68k_access;

    // M68k::Bus: RAM em 0x000000 (espelhada), registradores em 0x100000 (4 KiB espelhados).
    uint8_t read8(uint32_t addr) override { return static_cast<uint8_t>(read68k(addr, 1)); }
    uint16_t read16(uint32_t addr) override { return static_cast<uint16_t>(read68k(addr, 2)); }
    void write8(uint32_t addr, uint8_t value) override { write68k(addr, 1, value); }
    void write16(uint32_t addr, uint16_t value) override { write68k(addr, 2, value); }

private:
    uint32_t read68k(uint32_t addr, int bytes);
    void write68k(uint32_t addr, int bytes, uint32_t value);
    uint16_t word(uint32_t offset) const { return (uint16_t(regs_[offset]) << 8) | regs_[offset + 1]; }
    void set_word(uint32_t offset, uint16_t v) { regs_[offset] = v >> 8; regs_[offset + 1] = v & 0xFF; }
    void write_register_byte(uint32_t offset, uint8_t value);
    void set_pending(uint16_t bits);
    void sample_tick();
    void tick_timer(Reg timer, uint16_t bit);
    int cpu_level() const;
    void key_on_off();
    void advance_slots();
    void update_monitor();
    void step_envelope(int index);
    void mix_audio();
    void run_dsp();
    // Registradores de trabalho do DSP (TEMP, MEMS, MIXS): cada valor ocupa duas palavras (a primeira guarda os bits baixos).
    bool dsp_work_byte(uint32_t offset, int32_t*& word, int& shift, uint32_t& mask);
    uint8_t read_register_byte(uint32_t offset);
    int16_t fetch_pcm(uint32_t base_address, int control, uint32_t index) const;
    void refresh_interrupts();

    std::vector<uint8_t> ram_, regs_;
    M68k cpu_{*this};
    bool running_ = false;
    uint64_t samples_ = 0;
    int64_t sample_credit_ = 0, cpu_credit_ = 0;
    enum class Eg : uint8_t { kAttack, kDecay1, kDecay2, kRelease };
    struct Slot {
        bool playing = false;     // posição avançando (até o fim sem laço ou o fim do release)
        bool key = false;         // KYONB aplicado pelo último KYONEX
        bool reversed = false;    // sentido de leitura nos laços reverso/alternado
        Eg eg = Eg::kRelease;
        uint16_t attenuation = 0x3FF;   // 10 bits, 0 = volume máximo (passos de 0,09375 dB)
        int64_t position = 0;     // posição de leitura em amostras, com 20 bits fracionários
    };
    Slot slots_[kSlotCount];
    uint32_t eg_counter_ = 0, noise_ = 1;
    int16_t ext_left_ = 0, ext_right_ = 0;
    bool audio_enabled_ = false;
    std::vector<int16_t> audio_;         // anel de quadros estéreo
    static constexpr size_t kAudioRingMask = kAudioBufferFrames - 1;
    uint64_t audio_read_ = 0, audio_write_ = 0;   // contadores livres de quadros lidos/escritos
    // Estado interno do DSP (os registradores de programa, coeficientes e endereços ficam em regs_).
    struct Dsp {
        int32_t temp[kDspSteps] = {}, mems[32] = {}, mixs[16] = {};   // 24, 24 e 20 bits
        int32_t inputs = 0, y_reg = 0;                                // 24 bits
        uint32_t sft_reg = 0;                                         // 26 bits
        uint16_t frc_reg = 0, adrs_reg = 0, mdec_ct = 0;
        uint32_t rw_addr = 0, read_value = 0;
        uint16_t write_value = 0;
        uint8_t read_pending = 0;   // 0 nada, 1 leitura em ponto flutuante, 2 leitura sem conversão (NOFL)
        bool write_pending = false;
    } dsp_;
    bool dsp_active_ = false;   // algum byte do microprograma já foi escrito com valor não nulo
    uint16_t active_cpu_ = 0, active_main_ = 0;   // pendências habilitadas já sinalizadas (detecção de borda)
};

}  // namespace audio
