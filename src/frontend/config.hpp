// Configuração do port (dracula-x.ini): modelo, leitura tolerante, gravação e geometria da imagem na janela.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "frontend/input.hpp"

namespace frontend {

enum class Aspect { kOriginal, k4x3, kStretch };
enum class Filter { kSharp, kSmooth };
enum class Latency { kLow, kMedium, kHigh };

struct Config {
    // Vídeo
    bool fullscreen = false;        // tela cheia sem borda
    bool integer_scale = true;
    Aspect aspect = Aspect::kOriginal;
    Filter filter = Filter::kSharp;
    bool scanlines = false;
    bool vsync = false;
    int window_scale = 3;           // 1..8: tamanho inicial da janela
    // Áudio
    int volume = 100;               // 0..100, em passos de 5
    bool mute = false;
    Latency latency = Latency::kMedium;
    // Controles
    Bindings bindings = default_bindings();
    int deadzone = 35;              // % do curso do analógico (5..60)
    // Save states
    int slot = 0;                   // 0..7

    friend bool operator==(const Config&, const Config&) = default;
};

inline constexpr int kSlotCount = 8;

// Fila alvo do áudio, em quadros do núcleo: baixa 2, média 3, alta 6.
int latency_frames(Latency latency);

// Lê o texto do arquivo: chaves desconhecidas e valores inválidos são ignorados (o campo fica no padrão).
Config parse_config(std::string_view text);
std::string serialize_config(const Config& config);
Config load_config(const std::filesystem::path& file);                     // arquivo ausente/ilegível: padrão
bool save_config(const std::filesystem::path& file, const Config& config);   // troca atômica (.tmp + rename)

struct Rect { int x = 0, y = 0, w = 0, h = 0; };
// Retângulo da imagem do jogo (frame_w x frame_h) numa janela de win_w x win_h, conforme proporção e escala inteira.
Rect game_rect(const Config& config, int win_w, int win_h, int frame_w = 320, int frame_h = 224);

}  // namespace frontend
