#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "saturn/bus.hpp"
#include "saturn/scu.hpp"
#include "saturn/state.hpp"

namespace saturn {

// System Manager & Peripheral Control (0x00100000): comandos de sistema, relógio e entrada. Os registradores são bytes em offsets
// ímpares (IREG0–6, COMREG, OREG0–31, SR, SF…). O comando é executado na escrita de COMREG; SF volta a 0 e a interrupção
// System Manager é gerada no SCU.
class Smpc : public IoDevice {
public:
    explicit Smpc(Scu& scu);

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;

    std::function<void(bool on)> on_slave_power;   // SSHON / SSHOFF
    std::function<void(bool on)> on_sound_power;   // SNDON / SNDOFF
    std::function<void()> on_clock_change;         // CKCHG352/320 (após desligar escravo e som): o dono reinicia o SCU
    std::function<void()> on_nmi;                  // NMI no mestre (fim do CKCHG, NMIREQ)

    // O comando só termina (SF=0 e interrupção System Manager) depois de kCommandCycles: o software ainda precisa registrar que espera a interrupção.
    static constexpr uint32_t kCommandCycles = 20000;   // ≈ 0,7 ms a 28,6 MHz [inf]
    // O CKCHG espera alguns VBlanks antes da NMI (Mednafen: ~3 quadros); o mestre fica em SLEEP até lá.
    static constexpr uint32_t kClockChangeCycles = 1'500'000;
    void advance(uint64_t cycles);

    void save(StateWriter& w) const;
    void load(StateReader& r);

    void set_pad(uint16_t buttons) { pad_ = buttons; }
    void set_area_code(uint8_t code) { area_code_ = code; }
    const std::vector<uint8_t>& unknown_commands() const { return unknown_commands_; }

private:
    uint8_t read_byte(uint32_t offset) const;
    void write_byte(uint32_t offset, uint8_t value);
    void execute(uint8_t command);
    void intback();
    void send_peripheral_data();
    void set_slave(bool on);   // muda o estado e avisa o dono
    void set_sound(bool on);
    void clear_oregs();

    Scu& scu_;
    uint8_t regs_[0x80] = {};
    uint16_t pad_ = 0xFFFF;
    uint8_t area_code_ = 0x01;  // Japão
    std::vector<uint8_t> unknown_commands_;
    bool peripheral_pending_ = false;  // INTBACK de status já respondido; o pad sai quando o software pedir a continuação
    uint32_t completion_cycles_ = 0;   // ciclos até concluir o comando em andamento; 0 = ocioso
    bool nmi_on_completion_ = false;   // CKCHG/NMIREQ: a conclusão gera NMI no mestre
    // Estado do sistema informado no INTBACK (OREG0, OREG10–15).
    bool clock_352_ = false, slave_on_ = false, sound_on_ = false, cd_on_ = true, reset_enabled_ = false;
    uint8_t smem_[4] = {};
};

}  // namespace saturn
