// Cenários animados da galeria da trilha sonora, desenhados em software, do zero, por procedimentos (gradientes, silhuetas,
// partículas): nada vem do disco nem de outras obras. Tudo é função determinística de (tema, semente, tempo, energia), então os
// testes e as capturas de diagnóstico saem sempre iguais. Sem SDL.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "video/frame.hpp"

namespace frontend {

enum class Theme { kGeneric, kCastle, kLibrary, kChapel, kCaverns, kClockTower, kGraveyard, kInverted };
inline constexpr int kThemeCount = 8;

const char* theme_name(Theme theme);   // nome em português, para a interface

struct SceneParams {
    Theme theme = Theme::kGeneric;
    uint32_t seed = 0;     // varia a paleta/arranjo dentro do tema (ex.: número da faixa)
    double time = 0;       // segundos desde que a galeria abriu
    float energy = 0;      // 0..1: intensidade da música tocando (pulsa luzes e brilhos)
};

// Preenche o quadro inteiro (opaco) com o cenário do tema.
void render_scene(video::Frame& frame, const SceneParams& params);

inline constexpr int kSpectrumBands = 24;
using Spectrum = std::array<float, kSpectrumBands>;   // cada banda 0..1

// Níveis por banda (escala logarítmica de ~60 Hz a ~12 kHz) dos últimos ~1024 quadros de PCM estéreo intercalado.
Spectrum compute_spectrum(std::span<const int16_t> stereo, int sample_rate);

// Barras do espectro no retângulo dado, nas cores do tema. Mesmo parado (tudo zero) mexe de leve, para a faixa não ficar morta.
void draw_spectrum(video::Frame& frame, int x, int y, int w, int h, const Spectrum& bands, Theme theme, double time);

}  // namespace frontend
