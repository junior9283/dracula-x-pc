// Fonte bitmap própria (desenhada do zero para este projeto): células de 6x12 pixels, ASCII imprimível e os acentos do português.
#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>

#include "video/frame.hpp"

namespace frontend {

inline constexpr int kGlyphWidth = 6, kGlyphHeight = 12;

// Uma linha por elemento; os bits 0x10..0x01 são as colunas 0..4 (a coluna 5 é sempre o espaço entre letras).
using GlyphRows = std::array<uint8_t, kGlyphHeight>;

// Glifo de um ponto de código Unicode; o que a fonte não tem vira '?'.
GlyphRows glyph_rows(char32_t codepoint);
bool has_glyph(char32_t codepoint);

// UTF-8 -> pontos de código (bytes inválidos viram U+FFFD, que desenha como '?').
std::u32string decode_utf8(std::string_view text);
int text_width(std::string_view utf8);

// Mistura src (RGBA com alfa) sobre dst.
inline uint32_t blend_pixel(uint32_t dst, uint32_t src) {
    const uint32_t sa = src >> 24, da = dst >> 24;
    if (sa == 255) return src;
    if (sa == 0) return dst;
    // "Sobre" com alfa: o destino pode ser transparente (overlay) ou opaco (quadro do jogo).
    const uint32_t weight_dst = da * (255 - sa) / 255, out_a = sa + weight_dst;
    uint32_t out = out_a << 24;
    if (da == 255) {   // destino opaco (caso comum): out_a = 255, divisão por constante
        for (int shift = 0; shift <= 16; shift += 8)
            out |= ((((src >> shift) & 0xFF) * sa + ((dst >> shift) & 0xFF) * weight_dst + 127) / 255) << shift;
        return out;
    }
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t d = (dst >> shift) & 0xFF, s = (src >> shift) & 0xFF;
        out |= ((s * sa + d * weight_dst + out_a / 2) / out_a) << shift;
    }
    return out;
}
void fill_rect(video::Frame& frame, int x, int y, int w, int h, uint32_t color);
// Desenha o texto com o canto superior esquerdo em (x, y); não passa de max_x (exclusivo). Devolve o x depois do último glifo.
int draw_text(video::Frame& frame, int x, int y, std::string_view utf8, uint32_t color, int max_x = INT_MAX);

}  // namespace frontend
