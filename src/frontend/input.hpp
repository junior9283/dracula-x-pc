// Entrada: botões do Saturn, mapeamentos configuráveis (teclado e controle) e repetição de teclas para a navegação do menu.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace frontend {

enum Button : int { kUp, kDown, kLeft, kRight, kStart, kA, kB, kC, kX, kY, kZ, kL, kR, kButtonCount };

// Bit do controle digital do Saturn (1 = pressionado aqui; o SMPC recebe o complemento).
constexpr uint16_t pad_bit(int button) {
    constexpr uint16_t kBits[kButtonCount] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0800, 0x0400, 0x0100, 0x0200, 0x0040, 0x0020, 0x0010,
                                              0x0008, 0x0080};
    return kBits[button];
}
const char* button_label(int button);   // "Cima", "Baixo", ..., "Start", "A", ...

// Códigos de entrada do controle: 0..40 são os SDL_GamepadButton; os gatilhos analógicos têm códigos próprios.
inline constexpr int kUnbound = -1;
inline constexpr int kPadLeftTrigger = 1000, kPadRightTrigger = 1001;
inline constexpr int kMaxScancode = 511, kMaxPadButton = 40;
inline constexpr int kScancodeEscape = 41;   // SDL_SCANCODE_ESCAPE (valores de teclado = códigos USB HID, estáveis)
bool valid_key_code(int code);
bool valid_pad_code(int code);

struct Bindings {
    std::array<int, kButtonCount> keys{};   // um scancode do SDL por botão do Saturn (ou kUnbound)
    std::array<int, kButtonCount> pad{};    // um código de entrada do controle por botão (ou kUnbound)

    friend bool operator==(const Bindings&, const Bindings&) = default;
};
Bindings default_bindings();
std::string pad_input_name(int code);   // "Sul (A)", "Gatilho esquerdo", ...

// Junta as teclas/botões pressionados nos bits do Saturn. key_down(scancode) e pad_down(código) dizem o estado atual.
template <class KeyFn, class PadFn>
uint16_t map_inputs(const Bindings& bindings, KeyFn&& key_down, PadFn&& pad_down) {
    uint16_t pressed = 0;
    for (int b = 0; b < kButtonCount; ++b) {
        if ((bindings.keys[b] != kUnbound && key_down(bindings.keys[b])) || (bindings.pad[b] != kUnbound && pad_down(bindings.pad[b])))
            pressed |= pad_bit(b);
    }
    return pressed;
}

// Analógico esquerdo como direcional. Eixos de -32768 a 32767 (y positivo = baixo); zona morta em porcentagem do curso.
uint16_t stick_bits(int16_t x, int16_t y, int deadzone_percent);
constexpr int deadzone_raw(int percent) { return percent * 32767 / 100; }

// Repetição de uma tecla segurada (navegação do menu pelo controle): dispara ao apertar, depois de delay e a cada interval.
class Repeater {
public:
    Repeater(uint64_t delay_ms = 400, uint64_t interval_ms = 90) : delay_(delay_ms), interval_(interval_ms) {}
    bool update(bool down, uint64_t now_ms);

private:
    uint64_t delay_, interval_, next_ = 0;
    bool held_ = false;
};

}  // namespace frontend
