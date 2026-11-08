#include "frontend/input.hpp"

namespace frontend {

const char* button_label(int button) {
    static constexpr const char* kLabels[kButtonCount] = {"Cima", "Baixo", "Esquerda", "Direita", "Start", "A", "B", "C", "X", "Y", "Z", "L", "R"};
    return button >= 0 && button < kButtonCount ? kLabels[button] : "?";
}

bool valid_key_code(int code) { return code == kUnbound || (code >= 4 && code <= kMaxScancode); }   // 0..3 são "sem tecla" no SDL
bool valid_pad_code(int code) {
    return code == kUnbound || (code >= 0 && code <= kMaxPadButton) || code == kPadLeftTrigger || code == kPadRightTrigger;
}

Bindings default_bindings() {
    Bindings b;
    // Teclado (scancodes do SDL): setas, Enter = Start, Z/X/C = A/B/C, A/S/D = X/Y/Z, Q/W = L/R.
    b.keys = {82, 81, 80, 79, 40, 29, 27, 6, 4, 22, 7, 20, 26};
    // Controle (SDL_GamepadButton): direcional, Start, Sul/Leste/Oeste = A/B/C, Norte = Y, ombros = L/R; gatilhos = X/Z.
    b.pad = {11, 12, 13, 14, 6, 0, 1, 2, kPadLeftTrigger, 3, kPadRightTrigger, 9, 10};
    return b;
}

std::string pad_input_name(int code) {
    static constexpr const char* kNames[] = {"Sul (A)", "Leste (B)", "Oeste (X)", "Norte (Y)", "Back", "Guide", "Start", "Analógico esq.",
                                             "Analógico dir.", "Ombro esq.", "Ombro dir.", "Direcional cima", "Direcional baixo",
                                             "Direcional esq.", "Direcional dir."};
    if (code == kUnbound) return "(nenhum)";
    if (code == kPadLeftTrigger) return "Gatilho esq.";
    if (code == kPadRightTrigger) return "Gatilho dir.";
    if (code >= 0 && code < int(sizeof(kNames) / sizeof(kNames[0]))) return kNames[code];
    return "Botão " + std::to_string(code);
}

uint16_t stick_bits(int16_t x, int16_t y, int deadzone_percent) {
    const int dead = deadzone_raw(deadzone_percent);
    uint16_t pressed = 0;
    if (x > dead) pressed |= pad_bit(kRight);
    if (x < -dead) pressed |= pad_bit(kLeft);
    if (y > dead) pressed |= pad_bit(kDown);
    if (y < -dead) pressed |= pad_bit(kUp);
    return pressed;
}

bool Repeater::update(bool down, uint64_t now_ms) {
    if (!down) { held_ = false; return false; }
    if (!held_) {
        held_ = true;
        next_ = now_ms + delay_;
        return true;
    }
    if (now_ms < next_) return false;
    next_ = now_ms + interval_;
    return true;
}

}  // namespace frontend
