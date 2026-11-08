#include "frontend/scenes.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "frontend/font.hpp"

namespace frontend {
namespace {

using video::rgba;

constexpr float kPi = 3.14159265358979f;

// ---- Aleatório determinístico e cores ----

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}
// Número estável em [0,1) para (semente, índice, canal).
float rnd(uint32_t seed, uint32_t i, uint32_t k = 0) { return float(hash32(seed * 0x9E3779B1u + i * 0x85EBCA6Bu + k * 0xC2B2AE35u) >> 8) / 16777216.0f; }

struct Rgb { float r, g, b; };
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
uint32_t pack(Rgb c, float alpha = 1.0f) {
    const auto q = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return rgba(q(c.r), q(c.g), q(c.b), q(alpha));
}
Rgb hsv(float h, float s, float v) {
    h = h - std::floor(h);
    const float k = h * 6.0f;
    const int i = int(k);
    const float f = k - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}
// Norma euclidiana via sqrt em double: mais rápida que std::hypot e, para estas coordenadas pequenas, com o mesmo resultado em float.
inline float hyp(float a, float b) { return float(std::sqrt(double(a) * a + double(b) * b)); }
float smooth01(float v) { v = std::clamp(v, 0.0f, 1.0f); return v * v * (3 - 2 * v); }

// Cintilar de vela/chama: soma de senos incomensuráveis, 0..1.
float flicker(double t, float phase) {
    return 0.5f + 0.25f * float(std::sin(t * 9.1 + phase)) + 0.15f * float(std::sin(t * 15.7 + phase * 2.3)) + 0.10f * float(std::sin(t * 3.3 + phase * 0.7));
}

// ---- Pincel ----

struct Painter {
    video::Frame& f;
    int w, h;
    explicit Painter(video::Frame& frame) : f(frame), w(frame.width), h(frame.height) {}

    void put(int x, int y, uint32_t c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint32_t& p = f.pixels[size_t(y) * w + x];
        p = blend_pixel(p, c);
    }
    void rect(int x, int y, int rw, int rh, uint32_t c) { fill_rect(f, x, y, rw, rh, c); }

    void gradient(int y0, int y1, Rgb top, Rgb bottom) {
        for (int y = std::max(y0, 0); y < std::min(y1, h); ++y) {
            const uint32_t c = pack(mix(top, bottom, y1 > y0 + 1 ? float(y - y0) / float(y1 - y0 - 1) : 0.0f));
            std::fill_n(f.pixels.begin() + size_t(y) * w, w, c);
        }
    }
    // Disco com borda suavizada.
    void disc(float cx, float cy, float r, Rgb c, float alpha = 1.0f) {
        for (int y = int(std::floor(cy - r - 1)); y <= int(std::ceil(cy + r + 1)); ++y)
            for (int x = int(std::floor(cx - r - 1)); x <= int(std::ceil(cx + r + 1)); ++x) {
                const float d = hyp(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
                const float cover = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
                if (cover > 0) put(x, y, pack(c, alpha * cover));
            }
    }
    // Brilho radial suave (queda quadrática).
    void glow(float cx, float cy, float r, Rgb c, float strength) {
        for (int y = int(std::floor(cy - r)); y <= int(std::ceil(cy + r)); ++y)
            for (int x = int(std::floor(cx - r)); x <= int(std::ceil(cx + r)); ++x) {
                const float d = hyp(float(x) + 0.5f - cx, float(y) + 0.5f - cy) / r;
                if (d >= 1) continue;
                const float a = strength * (1 - d) * (1 - d);
                if (a > 0.004f) put(x, y, pack(c, a));
            }
    }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, Rgb c, float alpha = 1.0f) {
        const int minx = int(std::floor(std::min({x0, x1, x2}))), maxx = int(std::ceil(std::max({x0, x1, x2})));
        const int miny = int(std::floor(std::min({y0, y1, y2}))), maxy = int(std::ceil(std::max({y0, y1, y2})));
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-4f) return;
        const uint32_t color = pack(c, alpha);
        for (int y = std::max(miny, 0); y <= std::min(maxy, h - 1); ++y)
            for (int x = std::max(minx, 0); x <= std::min(maxx, w - 1); ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float a = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                const float b = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                if (a >= 0 && b >= 0 && a + b <= 1) put(x, y, color);
            }
    }
    // Segmento com espessura (distância ao segmento).
    void line(float x0, float y0, float x1, float y1, float half_width, Rgb c, float alpha = 1.0f) {
        const float dx = x1 - x0, dy = y1 - y0, len2 = std::max(dx * dx + dy * dy, 1e-6f);
        for (int y = int(std::floor(std::min(y0, y1) - half_width - 1)); y <= int(std::ceil(std::max(y0, y1) + half_width + 1)); ++y)
            for (int x = int(std::floor(std::min(x0, x1) - half_width - 1)); x <= int(std::ceil(std::max(x0, x1) + half_width + 1)); ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float u = std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f);
                const float d = hyp(px - (x0 + u * dx), py - (y0 + u * dy));
                const float cover = std::clamp(half_width - d + 0.5f, 0.0f, 1.0f);
                if (cover > 0) put(x, y, pack(c, alpha * cover));
            }
    }
    // Faixa de relevo (colinas, muros): preenche da curva até a borda 'to_bottom' (ou até o topo, se false).
    template <class HeightFn>
    void ridge(HeightFn height, Rgb c, bool to_bottom = true, float alpha = 1.0f) {
        const uint32_t color = pack(c, alpha);
        for (int x = 0; x < w; ++x) {
            const int y = int(height(float(x)));
            if (to_bottom) rect(x, y, 1, h - y, color);
            else rect(x, 0, 1, y, color);
        }
    }
    // Neblina em faixas que deslizam: alfa varia ao longo de x e cai suave no sentido vertical.
    void fog(float y_center, float thickness, Rgb c, float max_alpha, double t, float speed, float phase) {
        std::vector<float> noise(static_cast<size_t>(w), 0.0f);   // não depende de y: um cálculo por coluna
        for (int x = 0; x < w; ++x) {
            const float n = 0.5f + 0.3f * std::sin(float(x) * 0.021f + float(t) * speed + phase) + 0.2f * std::sin(float(x) * 0.057f - float(t) * speed * 1.7f + phase * 2);
            noise[size_t(x)] = std::clamp(n, 0.0f, 1.0f);
        }
        for (int y = int(y_center - thickness); y <= int(y_center + thickness); ++y) {
            const float vert = 1 - std::fabs(float(y) - y_center) / thickness;
            if (vert <= 0) continue;
            for (int x = 0; x < w; ++x) {
                const float a = max_alpha * vert * vert * noise[size_t(x)];
                if (a > 0.004f) put(x, y, pack(c, a));
            }
        }
    }
    void stars(uint32_t seed, int count, int max_y, double t, float brightness) {
        for (int i = 0; i < count; ++i) {
            const int x = int(rnd(seed, i, 1) * w), y = int(rnd(seed, i, 2) * max_y);
            const float tw = 0.55f + 0.45f * std::sin(float(t) * (1.0f + rnd(seed, i, 3) * 2.5f) + rnd(seed, i, 4) * 6.28f);
            const float a = brightness * tw * (0.35f + 0.65f * rnd(seed, i, 5));
            put(x, y, pack({0.9f, 0.92f, 1.0f}, a));
            if (rnd(seed, i, 6) > 0.85f) {   // estrelas maiores em cruz
                put(x - 1, y, pack({0.8f, 0.85f, 1.0f}, a * 0.5f)); put(x + 1, y, pack({0.8f, 0.85f, 1.0f}, a * 0.5f));
                put(x, y - 1, pack({0.8f, 0.85f, 1.0f}, a * 0.5f)); put(x, y + 1, pack({0.8f, 0.85f, 1.0f}, a * 0.5f));
            }
        }
    }
    void vignette(float strength) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float dx = (float(x) / w - 0.5f) * 2, dy = (float(y) / h - 0.5f) * 2;
                const float a = strength * std::clamp((dx * dx + dy * dy - 0.35f) / 1.3f, 0.0f, 1.0f);
                if (a > 0.01f) put(x, y, pack({0, 0, 0}, a));
            }
    }
};

// ---- Peças reutilizáveis ----

// Morcego: corpo pequeno e duas asas que batem.
void bat(Painter& p, float x, float y, float size, double t, float phase, Rgb c) {
    const float flap = std::sin(float(t) * 11.0f + phase);
    const float wing_y = y - flap * size * 0.9f;
    p.tri(x, y - size * 0.3f, x - size * 2.2f, wing_y, x - size * 0.7f, y + size * 0.55f, c);
    p.tri(x, y - size * 0.3f, x + size * 2.2f, wing_y, x + size * 0.7f, y + size * 0.55f, c);
    p.disc(x, y, size * 0.7f, c);
    p.tri(x - size * 0.5f, y - size * 0.4f, x - size * 0.2f, y - size * 1.1f, x, y - size * 0.4f, c);
    p.tri(x + size * 0.5f, y - size * 0.4f, x + size * 0.2f, y - size * 1.1f, x, y - size * 0.4f, c);
}

void bats(Painter& p, uint32_t seed, int count, float y_min, float y_max, double t, Rgb c) {
    for (int i = 0; i < count; ++i) {
        const float speed = 18.0f + rnd(seed, i, 1) * 30.0f, size = 2.0f + rnd(seed, i, 2) * 2.2f;
        const float span = float(p.w) + 80;
        float x = std::fmod(rnd(seed, i, 3) * span + float(t) * speed * (i % 2 ? 1 : -1), span);
        if (x < 0) x += span;
        const float y = y_min + (y_max - y_min) * rnd(seed, i, 4) + std::sin(float(t) * 1.3f + i) * 8.0f;
        bat(p, x - 40, y, size, t, rnd(seed, i, 5) * 6.28f, c);
    }
}

// Silhueta de castelo (torres com telhados, muralhas, janelas acesas). 'sign' = +1 em pé, -1 de cabeça para baixo (pendurado
// no topo, com 'base_y' como teto).
void castle(Painter& p, float cx, float base_y, float s, float sign, Rgb body, Rgb window, double t, uint32_t seed) {
    const auto Y = [&](float up) { return base_y - sign * up * s; };
    const auto block = [&](float dx0, float dx1, float height) {   // retângulo do chão até 'height'
        const float ya = Y(0), yb = Y(height);
        p.rect(int(std::lround(cx + dx0 * s)), int(std::lround(std::min(ya, yb))), int(std::lround((dx1 - dx0) * s)), int(std::lround(std::fabs(yb - ya))), pack(body));
    };
    const auto roof = [&](float dx0, float dx1, float height, float peak) {
        p.tri(cx + (dx0 - 1.5f) * s, Y(height), cx + (dx1 + 1.5f) * s, Y(height), cx + (dx0 + dx1) * 0.5f * s, Y(height + peak), body);
    };
    const auto battlements = [&](float dx0, float dx1, float height) {
        for (float x = dx0; x + 3 <= dx1; x += 6) {
            const float ya = Y(height), yb = Y(height + 4);
            p.rect(int(std::lround(cx + x * s)), int(std::lround(std::min(ya, yb))), std::max(1, int(std::lround(3 * s))), int(std::lround(std::fabs(yb - ya))), pack(body));
        }
    };
    const auto windows = [&](float dx0, float dx1, float bottom, float top, int idx) {
        for (float y = bottom; y <= top; y += 14) {
            for (float x = dx0 + 3; x + 3 <= dx1; x += 8) {
                const int k = idx * 31 + int(x) * 7 + int(y);
                if (rnd(seed, k, 9) < 0.35f) continue;
                const float lit = 0.55f + 0.45f * flicker(t * 0.6, rnd(seed, k, 1) * 20);
                const float ya = Y(y), yb = Y(y + 5);
                p.rect(int(std::lround(cx + x * s)), int(std::lround(std::min(ya, yb))), std::max(1, int(std::lround(2 * s))), std::max(1, int(std::lround(5 * s))), pack(window, lit));
            }
        }
    };
    block(-130, -42, 38); battlements(-130, -42, 38);                  // muralha esquerda
    block(78, 135, 32); battlements(78, 135, 32);                      // muralha direita
    block(-40, 40, 74); battlements(-40, 40, 74); windows(-40, 40, 14, 62, 1);   // corpo principal
    block(-14, 14, 118); roof(-14, 14, 118, 34); windows(-14, 14, 82, 108, 2);   // torre central
    block(-72, -50, 98); roof(-72, -50, 98, 28); windows(-72, -50, 60, 88, 3);   // torre esquerda
    block(52, 76, 88); roof(52, 76, 88, 26); windows(52, 76, 50, 78, 4);         // torre direita
    block(-100, -90, 62); roof(-100, -90, 62, 16);                              // torreta da muralha
    block(100, 110, 54); roof(100, 110, 54, 15);
}

// ---- Cenários ----

void scene_castle(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    p.gradient(0, p.h, {0.04f, 0.03f, 0.13f}, {0.45f, 0.14f, 0.22f});
    p.stars(s.seed, 70, int(H * 0.55f), s.time, 0.9f);
    const float mx = W * 0.74f, my = H * 0.24f;
    p.glow(mx, my, 70 + 8 * e, {0.9f, 0.85f, 1.0f}, 0.30f + 0.15f * e);
    p.disc(mx, my, 20, {0.95f, 0.93f, 0.85f});
    p.disc(mx - 6, my - 4, 4.5f, {0.82f, 0.80f, 0.74f}, 0.6f);   // crateras
    p.disc(mx + 7, my + 6, 3.0f, {0.84f, 0.82f, 0.76f}, 0.6f);
    // nuvens finas que cruzam a lua
    for (int i = 0; i < 3; ++i) {
        const float x = std::fmod(float(s.time) * (4 + i * 2) + i * 150, W + 160) - 80;
        p.rect(int(x), int(my - 6 + i * 9), 70 + i * 20, 3, pack({0.12f, 0.08f, 0.2f}, 0.55f));
    }
    p.ridge([&](float x) { return H * 0.78f + 9 * std::sin(x * 0.02f + 1) + 5 * std::sin(x * 0.05f); }, {0.07f, 0.04f, 0.1f}, true, 0.8f);
    castle(p, W * 0.40f, H * 0.80f, 1.1f, +1, {0.03f, 0.02f, 0.06f}, {1.0f, 0.78f, 0.35f}, s.time, s.seed);
    p.fog(H * 0.82f, 18, {0.55f, 0.35f, 0.5f}, 0.30f, s.time, 0.35f, 1.0f);
    p.ridge([&](float x) { return H * 0.90f + 5 * std::sin(x * 0.03f + 2) + 3 * std::sin(x * 0.11f); }, {0.02f, 0.01f, 0.04f});
    bats(p, s.seed, 7, H * 0.10f, H * 0.55f, s.time, {0.02f, 0.01f, 0.04f});
}

void scene_inverted(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    // céu em baixo: vermelho vivo no fundo da tela, escuro em cima
    p.gradient(0, p.h, {0.10f, 0.01f, 0.07f}, {0.75f, 0.12f, 0.16f});
    p.stars(s.seed, 60, int(H * 0.5f), s.time, 0.7f);
    const float mx = W * 0.80f, my = H * 0.66f;
    p.glow(mx, my, 80 + 10 * e, {1.0f, 0.3f, 0.25f}, 0.42f + 0.2f * e);
    p.disc(mx, my, 22, {1.0f, 0.55f, 0.45f});
    p.disc(mx + 5, my - 3, 14, {0.95f, 0.38f, 0.32f}, 0.5f);
    p.ridge([&](float x) { return H * 0.20f + 8 * std::sin(x * 0.025f + 4) + 4 * std::sin(x * 0.07f); }, {0.1f, 0.02f, 0.06f}, false);
    castle(p, W * 0.56f, H * 0.18f, 1.05f, -1, {0.04f, 0.0f, 0.03f}, {1.0f, 0.6f, 0.3f}, s.time, s.seed);
    p.ridge([&](float x) { return H * 0.10f + 5 * std::sin(x * 0.04f + 1) + 2 * std::sin(x * 0.13f); }, {0.03f, 0.0f, 0.02f}, false);
    p.fog(H * 0.28f, 16, {0.9f, 0.25f, 0.3f}, 0.28f, s.time, 0.4f, 2.0f);
    // brasas que "caem" para cima (o mundo está invertido)
    for (int i = 0; i < 40; ++i) {
        const float x = rnd(s.seed, i, 1) * W + std::sin(float(s.time) * 0.8f + i) * 6;
        float y = std::fmod(rnd(s.seed, i, 2) * H - float(s.time) * (10 + rnd(s.seed, i, 3) * 16), H);
        if (y < 0) y += H;
        p.put(int(x), int(y), pack({1.0f, 0.65f, 0.3f}, 0.4f + 0.5f * flicker(s.time, float(i))));
    }
    bats(p, s.seed + 5, 6, H * 0.35f, H * 0.85f, s.time, {0.04f, 0.0f, 0.03f});
}

void scene_library(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    p.gradient(0, p.h, {0.10f, 0.06f, 0.05f}, {0.22f, 0.12f, 0.07f});
    constexpr int kBay = 80, kShelfTop = 26, kShelfH = 26;
    static const Rgb kSpines[] = {{0.55f, 0.12f, 0.12f}, {0.15f, 0.35f, 0.22f}, {0.12f, 0.2f, 0.45f}, {0.45f, 0.3f, 0.12f}, {0.35f, 0.15f, 0.4f},
                                  {0.6f, 0.45f, 0.18f}, {0.25f, 0.1f, 0.08f}, {0.2f, 0.4f, 0.4f}};
    for (int bay = 0; bay * kBay < p.w + kBay; ++bay) {
        const int bx = bay * kBay;
        if (bay == 2) {   // janela em arco no meio: luz do luar entrando
            const float wx = float(bx + kBay / 2);
            p.rect(bx + 10, 40, kBay - 20, 120, pack({0.06f, 0.07f, 0.16f}));
            p.disc(wx, 40, float(kBay - 20) / 2, {0.06f, 0.07f, 0.16f});
            p.glow(wx + 14, 62, 26, {0.8f, 0.85f, 1.0f}, 0.45f);
            p.disc(wx + 14, 62, 7, {0.92f, 0.93f, 0.98f});
            p.stars(s.seed, 14, 90, s.time, 0.8f);   // estrelas ficam sob o vitral; recortadas depois pelas barras abaixo
            for (int k = 0; k < 3; ++k) p.rect(bx + 10 + k * (kBay - 20) / 3 + 5, 22, 2, 138, pack({0.08f, 0.05f, 0.04f}));
            p.rect(bx + 10, 100, kBay - 20, 2, pack({0.08f, 0.05f, 0.04f}));
            continue;
        }
        p.rect(bx + 6, kShelfTop - 4, kBay - 12, 168, pack({0.07f, 0.04f, 0.03f}));   // fundo da estante
        for (int shelf = 0; shelf < 6; ++shelf) {
            const int sy = kShelfTop + shelf * kShelfH;
            int x = bx + 8;
            for (int b = 0; x < bx + kBay - 12; ++b) {
                const uint32_t id = uint32_t(bay * 977 + shelf * 131 + b);
                const int bw = 3 + int(rnd(s.seed, id, 1) * 4), bh = 15 + int(rnd(s.seed, id, 2) * 7);
                if (rnd(s.seed, id, 3) < 0.07f) { x += bw; continue; }   // vão
                Rgb c = kSpines[hash32(id + s.seed) % 8];
                const float shade = 0.7f + 0.3f * rnd(s.seed, id, 4);
                c = {c.r * shade, c.g * shade, c.b * shade};
                p.rect(x, sy + kShelfH - 4 - bh, bw, bh, pack(c));
                p.rect(x, sy + kShelfH - 4 - bh + 3, bw, 1, pack({0.8f, 0.65f, 0.3f}, 0.6f));   // filete dourado
                x += bw;
            }
            p.rect(bx + 6, sy + kShelfH - 4, kBay - 12, 4, pack({0.28f, 0.16f, 0.08f}));   // tábua
        }
        p.rect(bx, 0, 6, p.h, pack({0.16f, 0.09f, 0.05f}));   // pilastras entre as estantes
        p.rect(bx + 1, 0, 1, p.h, pack({0.3f, 0.18f, 0.1f}));
    }
    p.rect(0, 190, p.w, p.h - 190, pack({0.09f, 0.05f, 0.03f}));
    for (int i = 0; i < 12; ++i) p.rect(0, 192 + i * 3, p.w, 1, pack({0.2f, 0.1f, 0.05f}, 0.35f));
    // candelabros pendurados, balançando
    for (int i = 0; i < 3; ++i) {
        const float x = W * (0.2f + 0.3f * i) + std::sin(float(s.time) * 0.9f + i * 2) * 3, y = 34 + 14 * (i % 2);
        p.line(x, 0, x, y, 0.6f, {0.3f, 0.25f, 0.2f});
        p.rect(int(x) - 8, int(y), 16, 2, pack({0.55f, 0.4f, 0.15f}));
        for (int c = -1; c <= 1; ++c) {
            const float fl = flicker(s.time, float(i * 7 + c));
            p.glow(x + c * 6, y - 5, 28 + 10 * e, {1.0f, 0.7f, 0.3f}, 0.30f * fl + 0.1f * e);
            p.rect(int(x) + c * 6 - 1, int(y) - 5, 2, 5, pack({0.95f, 0.9f, 0.8f}));
            p.disc(x + c * 6, y - 7, 1.6f, {1.0f, 0.8f, 0.35f}, 0.7f + 0.3f * fl);
        }
    }
    // poeira em suspensão, subindo devagar
    for (int i = 0; i < 46; ++i) {
        const float x = rnd(s.seed, i, 7) * W + std::sin(float(s.time) * 0.5f + i) * 10;
        float y = std::fmod(rnd(s.seed, i, 8) * H - float(s.time) * (3 + rnd(s.seed, i, 9) * 6), H);
        if (y < 0) y += H;
        p.put(int(x), int(y), pack({1.0f, 0.9f, 0.7f}, 0.25f + 0.35f * flicker(s.time * 0.5, float(i))));
    }
    p.vignette(0.7f);
}

void scene_chapel(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    p.gradient(0, p.h, {0.04f, 0.03f, 0.10f}, {0.12f, 0.07f, 0.16f});
    const float cx = W * 0.72f, cy = H * 0.33f, R = 52;
    p.glow(cx, cy, 120, {0.7f, 0.5f, 0.9f}, 0.25f + 0.12f * e);
    // arcos ogivais ao fundo
    for (int i = 0; i < 4; ++i) {
        const float ax = W * (0.05f + 0.3f * i);
        p.tri(ax, 70, ax + 60, 70, ax + 30, 8, {0.08f, 0.05f, 0.14f});
        p.tri(ax + 8, 70, ax + 52, 70, ax + 30, 22, {0.03f, 0.02f, 0.07f});
    }
    // vitral em rosácea: 12 setores, anéis e chumbo escuro
    static const Rgb kPane[] = {{0.8f, 0.15f, 0.2f}, {0.2f, 0.4f, 0.85f}, {0.9f, 0.7f, 0.15f}, {0.2f, 0.65f, 0.35f}, {0.65f, 0.25f, 0.75f}, {0.95f, 0.45f, 0.15f}};
    for (int y = int(cy - R - 2); y <= int(cy + R + 2); ++y)
        for (int x = int(cx - R - 2); x <= int(cx + R + 2); ++x) {
            const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy, r = hyp(dx, dy);
            if (r > R + 3) continue;
            if (r > R) { p.put(x, y, pack({0.05f, 0.04f, 0.07f})); continue; }   // moldura
            float a = std::atan2(dy, dx) + float(s.time) * 0.05f;
            const float sector = (a + kPi) / (2 * kPi) * 12.0f;
            const int k = int(sector) % 12;
            const float edge_a = std::fabs(sector - std::round(sector)) * r * (2 * kPi / 12);   // distância (px) à divisa do setor
            const float ring = r / R * 4.0f, edge_r = std::fabs(ring - std::round(ring)) * R / 4.0f;
            const bool center = r < 7;
            Rgb c = center ? Rgb{1.0f, 0.9f, 0.5f} : kPane[(k + int(ring)) % 6];
            const float shimmer = 0.78f + 0.22f * std::sin(float(s.time) * 1.3f + k * 0.9f + ring * 2) + 0.2f * e;
            c = {c.r * shimmer, c.g * shimmer, c.b * shimmer};
            if (!center && (edge_a < 0.9f || edge_r < 0.9f)) c = {0.05f, 0.04f, 0.07f};
            p.put(x, y, pack(c));
        }
    // feixes de luz colorida descendo da rosácea
    for (int i = 0; i < 5; ++i) {
        const float sway = std::sin(float(s.time) * 0.4f + i) * 4, x0 = cx - 40 + i * 20, x1 = x0 - 70 + sway * 3;
        p.tri(x0, cy + 20, x0 + 14, cy + 20, x1 + 40, H * 0.9f, kPane[i], 0.07f + 0.04f * e);
        p.tri(x0, cy + 20, x1, H * 0.9f, x1 + 40, H * 0.9f, kPane[i], 0.07f + 0.04f * e);
    }
    // colunas da nave
    for (const float x : {W * 0.04f, W * 0.30f, W * 0.94f}) {
        p.rect(int(x), 0, 16, p.h, pack({0.1f, 0.07f, 0.16f}));
        p.rect(int(x) + 2, 0, 3, p.h, pack({0.25f, 0.18f, 0.35f}, 0.7f));
        p.rect(int(x) - 3, int(H * 0.88f), 22, 12, pack({0.08f, 0.05f, 0.12f}));
    }
    p.rect(0, int(H * 0.88f) + 10, p.w, p.h, pack({0.06f, 0.04f, 0.09f}));
    // reflexo colorido no piso e velas
    for (int i = 0; i < 6; ++i) p.glow(cx - 60 + i * 24, H * 0.93f, 22, kPane[i], 0.14f + 0.06f * e);
    for (int i = 0; i < 9; ++i) {
        const float x = 20 + i * 36.0f, fl = flicker(s.time, float(i * 3));
        p.rect(int(x), int(H * 0.88f) - 2, 3, 8, pack({0.92f, 0.88f, 0.78f}));
        p.glow(x + 1.5f, H * 0.88f - 4, 22 + 8 * e, {1.0f, 0.65f, 0.25f}, 0.30f * fl + 0.08f * e);
        p.disc(x + 1.5f, H * 0.88f - 4, 1.8f, {1.0f, 0.82f, 0.4f}, 0.6f + 0.4f * fl);
    }
    for (int i = 0; i < 40; ++i) {
        const float x = rnd(s.seed, i, 1) * W + std::sin(float(s.time) * 0.4f + i) * 8;
        float y = std::fmod(rnd(s.seed, i, 2) * H + float(s.time) * (2 + rnd(s.seed, i, 3) * 5), H);
        p.put(int(x), int(y), pack({0.9f, 0.85f, 1.0f}, 0.2f + 0.3f * flicker(s.time * 0.4, float(i))));
    }
    p.vignette(0.55f);
}

void scene_caverns(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    p.gradient(0, p.h, {0.03f, 0.08f, 0.14f}, {0.06f, 0.22f, 0.26f});
    // camadas de estalactites/estalagmites (fundo claro e pequeno, frente escura e grande)
    for (int layer = 0; layer < 3; ++layer) {
        const float shade = 0.20f - layer * 0.07f, scale = 0.6f + layer * 0.55f;
        const Rgb c = {0.03f + shade * 0.4f, 0.09f + shade * 0.6f, 0.12f + shade * 0.7f};
        for (float x = -10; x < W + 20;) {
            const uint32_t id = uint32_t(layer * 1000 + int(x));
            const float wd = (14 + rnd(s.seed, id, 1) * 22) * scale, len = (16 + rnd(s.seed, id, 2) * 60) * scale;
            p.tri(x, -1, x + wd, -1, x + wd * (0.3f + rnd(s.seed, id, 3) * 0.4f), len, c);
            x += wd * (0.7f + rnd(s.seed, id, 4) * 0.5f);
        }
        for (float x = -10; x < W + 20;) {
            const uint32_t id = uint32_t(layer * 1000 + 500 + int(x));
            const float wd = (14 + rnd(s.seed, id, 1) * 20) * scale, len = (10 + rnd(s.seed, id, 2) * 40) * scale;
            p.tri(x, H + 1, x + wd, H + 1, x + wd * 0.5f, H - len, c);
            x += wd * (0.8f + rnd(s.seed, id, 4) * 0.6f);
        }
    }
    // lago subterrâneo com reflexos
    const int lake = int(H * 0.56f);
    p.rect(0, lake, p.w, p.h - lake, pack({0.02f, 0.10f, 0.14f}, 0.85f));
    for (int i = 0; i < 18; ++i) {
        const float x = rnd(s.seed, i, 1) * W, y = lake + 3 + rnd(s.seed, i, 2) * (H - lake - 6);
        const float len = 8 + rnd(s.seed, i, 3) * 22, shimmer = 0.5f + 0.5f * std::sin(float(s.time) * 1.6f + i);
        p.rect(int(x + std::sin(float(s.time) + i) * 5), int(y), int(len), 1, pack({0.4f, 0.9f, 1.0f}, 0.10f + 0.2f * shimmer));
    }
    // cristais que pulsam com a música
    for (int i = 0; i < 6; ++i) {
        const float x = 160.0f + i * 28 + rnd(s.seed, i, 1) * 10, base = H * 0.56f + rnd(s.seed, i, 2) * 10;
        const Rgb c = (i % 2) ? Rgb{0.2f, 0.95f, 0.95f} : Rgb{0.85f, 0.35f, 0.95f};
        const float pulse = 0.5f + 0.3f * std::sin(float(s.time) * 1.5f + i * 1.7f) + 0.5f * e;
        p.glow(x, base - 14, 34, c, 0.22f * pulse);
        for (int k = -1; k <= 1; ++k) {
            const float hgt = 18 + (k == 0 ? 14 : 0) + rnd(s.seed, i * 3 + k + 5, 3) * 10;
            p.tri(x + k * 5 - 3, base, x + k * 5 + 3, base, x + k * 5 + k * 1.5f, base - hgt, {c.r * 0.6f, c.g * 0.6f, c.b * 0.6f});
            p.tri(x + k * 5, base, x + k * 5 + 3, base, x + k * 5 + k * 1.5f, base - hgt, c, 0.8f);
        }
    }
    // gotas que caem das pontas e fazem ondas no lago
    for (int i = 0; i < 5; ++i) {
        const float period = 2.0f + i * 0.6f, ph = std::fmod(float(s.time) / period + rnd(s.seed, i, 6), 1.0f);
        const float x = 150 + i * 34 + rnd(s.seed, i, 7) * 14;
        if (ph < 0.6f) {
            const float y = 20 + (lake - 24) * (ph / 0.6f) * (ph / 0.6f);
            p.line(x, y - 3, x, y, 0.6f, {0.6f, 0.9f, 1.0f}, 0.8f);
        } else {
            const float r = (ph - 0.6f) / 0.4f * 14;
            for (int a = 0; a < 40; ++a) {
                const float ang = a / 40.0f * 2 * kPi;
                p.put(int(x + std::cos(ang) * r), int(lake + 6 + std::sin(ang) * r * 0.25f), pack({0.7f, 0.95f, 1.0f}, 0.7f * (1 - (ph - 0.6f) / 0.4f)));
            }
        }
    }
    p.fog(H * 0.52f, 22, {0.4f, 0.7f, 0.8f}, 0.22f, s.time, 0.3f, 3.0f);
    bats(p, s.seed, 4, H * 0.15f, H * 0.5f, s.time, {0.01f, 0.03f, 0.05f});
    p.vignette(0.6f);
}

// Engrenagem: dentes quadrados em volta de um aro, furo e raios.
void cog(Painter& p, float cx, float cy, float r, int teeth, float rot, Rgb c, float alpha) {
    const float outer = r + r * 0.18f;
    for (int y = int(cy - outer - 1); y <= int(cy + outer + 1); ++y)
        for (int x = int(cx - outer - 1); x <= int(cx + outer + 1); ++x) {
            const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy, d = hyp(dx, dy);
            if (d > outer || d < r * 0.22f) continue;
            const float ang = std::atan2(dy, dx) - rot;
            const float tooth = std::fmod(ang / (2 * kPi) * float(teeth) + float(teeth) * 4, 1.0f);
            const bool in_tooth = tooth < 0.5f;
            if (d > r && !in_tooth) continue;
            const float spoke = std::fabs(std::sin((ang) * 3.0f));   // 6 raios com vazios entre eles
            if (d > r * 0.45f && d < r * 0.82f && spoke < 0.82f) continue;
            const float lit = 0.8f + 0.2f * std::cos(ang - 2.3f);
            p.put(x, y, pack({c.r * lit, c.g * lit, c.b * lit}, alpha));
        }
}

void scene_clock(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    p.gradient(0, p.h, {0.03f, 0.05f, 0.14f}, {0.12f, 0.12f, 0.28f});
    p.stars(s.seed, 80, int(H * 0.7f), s.time, 0.9f);
    const float cx = W * 0.75f, cy = H * 0.38f, R = 66;
    // engrenagens ao fundo
    cog(p, W * 0.12f, H * 0.78f, 44, 14, float(s.time) * 0.30f, {0.32f, 0.22f, 0.12f}, 0.95f);
    cog(p, W * 0.12f + 62, H * 0.78f - 38, 28, 9, -float(s.time) * 0.30f * 14.0f / 9.0f + 0.2f, {0.38f, 0.26f, 0.14f}, 0.95f);
    cog(p, W * 0.96f, H * 0.82f, 38, 12, -float(s.time) * 0.25f, {0.30f, 0.20f, 0.11f}, 0.95f);
    // mostrador
    p.glow(cx, cy, R + 40, {0.9f, 0.75f, 0.4f}, 0.20f + 0.1f * e);
    p.disc(cx, cy, R + 5, {0.55f, 0.4f, 0.15f});
    p.disc(cx, cy, R + 2, {0.85f, 0.68f, 0.28f});
    p.disc(cx, cy, R - 1, {0.1f, 0.1f, 0.2f});
    p.disc(cx, cy, R - 4, {0.82f, 0.78f, 0.62f}, 0.22f);
    for (int i = 0; i < 60; ++i) {
        const float a = i / 60.0f * 2 * kPi - kPi / 2, major = (i % 5 == 0) ? 1.0f : 0.0f;
        const float r0 = R - 6 - major * 7, r1 = R - 3;
        p.line(cx + std::cos(a) * r0, cy + std::sin(a) * r0, cx + std::cos(a) * r1, cy + std::sin(a) * r1, 0.5f + 0.6f * major, {0.95f, 0.85f, 0.55f}, major ? 1.0f : 0.6f);
    }
    // ponteiros: um giro do minutero a cada 24 s (acelerado, é só enfeite)
    const float minute = float(s.time) / 24.0f, hour = minute / 12.0f;
    const auto hand = [&](float turns, float len, float width, Rgb c) {
        const float a = turns * 2 * kPi - kPi / 2;
        p.line(cx - std::cos(a) * len * 0.15f, cy - std::sin(a) * len * 0.15f, cx + std::cos(a) * len, cy + std::sin(a) * len, width, c);
    };
    hand(hour + 0.1f, R * 0.5f, 2.2f, {0.95f, 0.85f, 0.5f});
    hand(minute, R * 0.78f, 1.5f, {0.95f, 0.85f, 0.5f});
    p.disc(cx, cy, 4, {0.9f, 0.7f, 0.25f});
    // pêndulo na parte de baixo
    const float ang = std::sin(float(s.time) * 2.2f) * 0.38f, px = W * 0.45f, py = H * 0.52f, len = H * 0.43f;
    const float bx = px + std::sin(ang) * len, by = py + std::cos(ang) * len;
    p.line(px, py, bx, by, 0.9f, {0.6f, 0.45f, 0.2f});
    p.glow(bx, by, 22, {1.0f, 0.8f, 0.35f}, 0.30f + 0.2f * e);
    p.disc(bx, by, 9, {0.9f, 0.7f, 0.25f});
    p.disc(bx - 2, by - 2, 4, {1.0f, 0.9f, 0.55f}, 0.7f);
    // chão de pedra e faíscas
    p.ridge([&](float x) { return H * 0.93f + 2 * std::sin(x * 0.2f); }, {0.04f, 0.04f, 0.1f});
    for (int i = 0; i < 24; ++i) {
        const float ph = std::fmod(float(s.time) * (0.3f + rnd(s.seed, i, 1) * 0.3f) + rnd(s.seed, i, 2), 1.0f);
        const float x = cx + (rnd(s.seed, i, 3) - 0.5f) * 220 + ph * 30, y = H * 0.9f - ph * 150;
        p.put(int(x), int(y), pack({1.0f, 0.8f, 0.4f}, (1 - ph) * 0.8f));
    }
    p.vignette(0.5f);
}

// Árvore seca: ramos recursivos balançando.
void branch(Painter& p, float x, float y, float angle, float len, int depth, double t, uint32_t seed, Rgb c) {
    if (depth == 0 || len < 2) return;
    const float sway = std::sin(float(t) * 0.9f + float(depth) + float(seed % 7)) * 0.03f * float(6 - depth > 0 ? 6 - depth : 1);
    const float a = angle + sway, x1 = x + std::cos(a) * len, y1 = y + std::sin(a) * len;
    p.line(x, y, x1, y1, 0.5f + depth * 0.45f, c);
    const float spread = 0.45f + rnd(seed, uint32_t(depth * 13 + int(len)), 1) * 0.3f;
    branch(p, x1, y1, a - spread, len * (0.68f + rnd(seed, uint32_t(depth), 2) * 0.12f), depth - 1, t, seed * 3 + 1, c);
    branch(p, x1, y1, a + spread * 0.9f, len * (0.68f + rnd(seed, uint32_t(depth), 3) * 0.12f), depth - 1, t, seed * 5 + 2, c);
}

void scene_graveyard(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    p.gradient(0, p.h, {0.03f, 0.07f, 0.10f}, {0.14f, 0.22f, 0.24f});
    p.stars(s.seed, 50, int(H * 0.5f), s.time, 0.7f);
    const float mx = W * 0.70f, my = H * 0.24f;
    p.glow(mx, my, 80 + 8 * e, {0.7f, 0.95f, 0.9f}, 0.28f + 0.1f * e);
    p.disc(mx, my, 17, {0.9f, 0.97f, 0.92f});
    p.ridge([&](float x) { return H * 0.62f + 12 * std::sin(x * 0.018f + 2) + 5 * std::sin(x * 0.06f); }, {0.05f, 0.1f, 0.12f});
    // árvores secas
    branch(p, W * 0.90f, H * 0.80f, -kPi / 2 - 0.1f, 42, 7, s.time, s.seed, {0.02f, 0.04f, 0.05f});
    branch(p, W * 0.12f, H * 0.78f, -kPi / 2 + 0.15f, 34, 6, s.time, s.seed + 9, {0.02f, 0.04f, 0.05f});
    p.fog(H * 0.66f, 20, {0.55f, 0.75f, 0.75f}, 0.32f, s.time, 0.30f, 0.5f);
    // lápides e cruzes em duas fileiras
    for (int row = 0; row < 2; ++row) {
        const float base = H * (0.78f + row * 0.13f), sc = 1.0f + row * 0.55f;
        const Rgb c = row ? Rgb{0.015f, 0.03f, 0.04f} : Rgb{0.04f, 0.08f, 0.09f};
        for (int i = 0; i < 9; ++i) {
            const uint32_t id = uint32_t(row * 50 + i);
            const float x = (i + 0.4f + rnd(s.seed, id, 1) * 0.5f) * W / 9.0f, hgt = (16 + rnd(s.seed, id, 2) * 12) * sc, wd = 10 * sc;
            if (rnd(s.seed, id, 3) < 0.35f) {   // cruz
                p.rect(int(x - 1.5f * sc), int(base - hgt), int(3 * sc), int(hgt), pack(c));
                p.rect(int(x - 5 * sc), int(base - hgt * 0.72f), int(10 * sc), int(3 * sc), pack(c));
            } else {                             // lápide arredondada
                p.rect(int(x - wd / 2), int(base - hgt + wd / 2), int(wd), int(hgt - wd / 2), pack(c));
                p.disc(x, base - hgt + wd / 2, wd / 2, c);
            }
        }
        p.rect(0, int(base) - 1, p.w, p.h - int(base) + 1, pack(c));
    }
    // fogos-fátuos
    for (int i = 0; i < 9; ++i) {
        const float x = std::fmod(rnd(s.seed, i, 1) * W + std::sin(float(s.time) * 0.5f + i * 2) * 30 + float(s.time) * 4, W);
        const float y = H * (0.45f + 0.35f * rnd(s.seed, i, 2)) + std::sin(float(s.time) * 0.8f + i) * 9;
        const float pulse = 0.5f + 0.5f * std::sin(float(s.time) * 2 + i) + 0.4f * e;
        p.glow(x, y, 16, {0.5f, 1.0f, 0.7f}, 0.35f * pulse);
        p.disc(x, y, 1.6f, {0.85f, 1.0f, 0.9f}, 0.9f);
    }
    p.fog(H * 0.90f, 14, {0.5f, 0.7f, 0.7f}, 0.28f, s.time, 0.45f, 4.0f);
    p.vignette(0.55f);
}

void scene_generic(Painter& p, const SceneParams& s) {
    const float W = float(p.w), H = float(p.h), e = s.energy;
    const float hue = 0.60f + 0.35f * rnd(s.seed, 0, 1);   // azul-violeta-vinho conforme a faixa
    const Rgb top = hsv(hue, 0.6f, 0.10f), bottom = hsv(hue + 0.06f, 0.55f, 0.32f), glowc = hsv(hue + 0.08f, 0.45f, 1.0f);
    p.gradient(0, p.h, top, bottom);
    p.stars(s.seed, 60, p.h, s.time, 0.7f);
    const float cx = W * (0.55f + 0.2f * rnd(s.seed, 0, 2)), cy = H * 0.40f;
    for (int i = 0; i < 6; ++i) {   // anéis que se expandem
        const float ph = std::fmod(float(s.time) * 0.15f + i / 6.0f, 1.0f), r = ph * 150;
        for (int a = 0; a < 160; ++a) {
            const float ang = a / 160.0f * 2 * kPi;
            p.put(int(cx + std::cos(ang) * r), int(cy + std::sin(ang) * r * 0.6f), pack(glowc, (1 - ph) * (0.5f + 0.4f * e)));
        }
    }
    for (int i = 0; i < 14; ++i) {   // raios girando
        const float a = float(s.time) * 0.08f + i * (2 * kPi / 14);
        p.tri(cx, cy, cx + std::cos(a) * 260, cy + std::sin(a) * 260, cx + std::cos(a + 0.07f) * 260, cy + std::sin(a + 0.07f) * 260, glowc, 0.05f + 0.04f * e);
    }
    p.glow(cx, cy, 60 + 14 * e, glowc, 0.45f);
    p.disc(cx, cy, 11, hsv(hue + 0.08f, 0.15f, 1.0f));
    for (int i = 0; i < 16; ++i) {   // orbes à deriva
        const float x = std::fmod(rnd(s.seed, i, 3) * W + float(s.time) * (3 + rnd(s.seed, i, 4) * 6), W);
        const float y = H * rnd(s.seed, i, 5) + std::sin(float(s.time) * 0.6f + i) * 14;
        p.glow(x, y, 10 + rnd(s.seed, i, 6) * 14, glowc, 0.18f + 0.1f * std::sin(float(s.time) + i));
    }
    p.ridge([&](float x) { return H * 0.86f + 6 * std::sin(x * 0.03f + float(s.seed)) + 3 * std::sin(x * 0.09f); }, hsv(hue, 0.7f, 0.05f));
    p.vignette(0.5f);
}

}  // namespace

const char* theme_name(Theme theme) {
    switch (theme) {
        case Theme::kCastle: return "Castelo";
        case Theme::kLibrary: return "Biblioteca";
        case Theme::kChapel: return "Capela";
        case Theme::kCaverns: return "Cavernas";
        case Theme::kClockTower: return "Torre do relógio";
        case Theme::kGraveyard: return "Cemitério";
        case Theme::kInverted: return "Castelo invertido";
        case Theme::kGeneric: break;
    }
    return "Genérico";
}

void render_scene(video::Frame& frame, const SceneParams& params) {
    Painter p(frame);
    switch (params.theme) {
        case Theme::kCastle: scene_castle(p, params); break;
        case Theme::kLibrary: scene_library(p, params); break;
        case Theme::kChapel: scene_chapel(p, params); break;
        case Theme::kCaverns: scene_caverns(p, params); break;
        case Theme::kClockTower: scene_clock(p, params); break;
        case Theme::kGraveyard: scene_graveyard(p, params); break;
        case Theme::kInverted: scene_inverted(p, params); break;
        case Theme::kGeneric: scene_generic(p, params); break;
    }
}

namespace {

// Janela de Hann e coeficientes de Goertzel só dependem de (frames, sample_rate): calculados uma vez e reaproveitados.
struct SpectrumTables {
    size_t frames = 0;
    int sample_rate = 0;
    std::array<float, 1024> hann{};
    std::array<float, kSpectrumBands> coeff{};
};

const SpectrumTables& spectrum_tables(size_t frames, int sample_rate) {
    thread_local SpectrumTables t;
    if (t.frames != frames || t.sample_rate != sample_rate) {
        t.frames = frames;
        t.sample_rate = sample_rate;
        for (size_t i = 0; i < frames; ++i) t.hann[i] = 0.5f - 0.5f * std::cos(2 * kPi * float(i) / float(frames - 1));
        for (int b = 0; b < kSpectrumBands; ++b) {
            const float freq = 60.0f * std::pow(12000.0f / 60.0f, float(b) / float(kSpectrumBands - 1));
            t.coeff[b] = 2 * std::cos(2 * kPi * freq / float(sample_rate));
        }
    }
    return t;
}

}  // namespace

Spectrum compute_spectrum(std::span<const int16_t> stereo, int sample_rate) {
    constexpr size_t kWindow = 1024;
    Spectrum bands{};
    const size_t frames = std::min(kWindow, stereo.size() / 2);
    if (frames < 64) return bands;
    const auto tail = stereo.last(frames * 2);
    const SpectrumTables& tables = spectrum_tables(frames, sample_rate);
    std::array<float, kWindow> mono;
    for (size_t i = 0; i < frames; ++i) mono[i] = float(int(tail[2 * i]) + int(tail[2 * i + 1])) * 0.5f / 32768.0f * tables.hann[i];
    for (int b = 0; b < kSpectrumBands; ++b) {
        const float coeff = tables.coeff[b];
        float s1 = 0, s2 = 0;
        for (size_t i = 0; i < frames; ++i) {   // Goertzel: potência numa frequência só
            const float s0 = mono[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const float amplitude = 4.0f * std::sqrt(std::max(power, 0.0f)) / float(frames);   // 4/N: compensa a janela de Hann
        const float db = 20 * std::log10(amplitude + 1e-7f) + 12.0f * float(b) / float(kSpectrumBands - 1);   // inclinação: música cai com a frequência
        bands[b] = std::clamp((db + 62.0f) / 50.0f, 0.0f, 1.0f);
    }
    return bands;
}

void draw_spectrum(video::Frame& frame, int x, int y, int w, int h, const Spectrum& bands, Theme theme, double time) {
    Rgb low = {0.9f, 0.3f, 0.35f}, high = {1.0f, 0.85f, 0.5f};
    switch (theme) {
        case Theme::kLibrary: low = {0.85f, 0.5f, 0.15f}; high = {1.0f, 0.9f, 0.55f}; break;
        case Theme::kChapel: low = {0.55f, 0.3f, 0.9f}; high = {1.0f, 0.75f, 0.9f}; break;
        case Theme::kCaverns: low = {0.1f, 0.6f, 0.75f}; high = {0.7f, 1.0f, 0.95f}; break;
        case Theme::kClockTower: low = {0.8f, 0.55f, 0.15f}; high = {1.0f, 0.92f, 0.6f}; break;
        case Theme::kGraveyard: low = {0.2f, 0.65f, 0.5f}; high = {0.8f, 1.0f, 0.85f}; break;
        case Theme::kInverted: low = {0.8f, 0.1f, 0.2f}; high = {1.0f, 0.6f, 0.4f}; break;
        case Theme::kGeneric: low = {0.4f, 0.5f, 0.95f}; high = {0.85f, 0.9f, 1.0f}; break;
        case Theme::kCastle: break;
    }
    fill_rect(frame, x - 2, y - 2, w + 4, h + 4, rgba(0, 0, 0, 120));
    const int n = kSpectrumBands, slot = w / n, bar_w = std::max(1, slot - 1);
    const int origin = x + (w - slot * n) / 2;
    constexpr int kSegment = 2, kGap = 1;   // barras em "luzinhas" empilhadas
    for (int i = 0; i < n; ++i) {
        const float idle = 0.04f + 0.03f * (0.5f + 0.5f * float(std::sin(time * 2.0 + i * 0.6)));
        const float level = std::max(bands[i], idle);
        const int segments = std::max(1, int(level * float(h / (kSegment + kGap)) + 0.5f));
        for (int sgm = 0; sgm < segments; ++sgm) {
            const float t = float(sgm) / float(h / (kSegment + kGap));
            const int sy = y + h - (sgm + 1) * (kSegment + kGap) + kGap;
            fill_rect(frame, origin + i * slot, sy, bar_w, kSegment, pack(mix(low, high, t)));
        }
    }
}

}  // namespace frontend
