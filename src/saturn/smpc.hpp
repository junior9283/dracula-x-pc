#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "saturn/bus.hpp"
#include "saturn/scu.hpp"

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

    void set_pad(uint16_t buttons) { pad_ = buttons; }
    void set_area_code(uint8_t code) { area_code_ = code; }
    const std::vector<uint8_t>& unknown_commands() const { return unknown_commands_; }

private:
    uint8_t read_byte(uint32_t offset) const;
    void write_byte(uint32_t offset, uint8_t value);
    void execute(uint8_t command);
    void intback();

    Scu& scu_;
    uint8_t regs_[0x80] = {};
    uint16_t pad_ = 0xFFFF;
    uint8_t area_code_ = 0x01;  // Japão
    std::vector<uint8_t> unknown_commands_;
};

}  // namespace saturn
