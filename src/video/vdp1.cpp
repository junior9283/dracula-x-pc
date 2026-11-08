#include "video/vdp1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace video {
namespace {

constexpr uint32_t kCommandBytes = 32;
constexpr int kMaxCommands = 8192;   // trava de segurança contra listas em laço

struct Vram {
    std::span<const uint8_t> mem;
    uint16_t u16(uint32_t a) const { a &= uint32_t(mem.size()) - 1; return uint16_t((mem[a] << 8) | mem[a + 1]); }
    uint8_t u8(uint32_t a) const { return mem[a & (uint32_t(mem.size()) - 1)]; }
};

struct Point { int x, y; };

uint16_t halve(uint16_t rgb) { return uint16_t(((rgb >> 1) & 0x3DEF) | 0x8000); }

struct Context {
    SpriteFrame& out;
    int local_x = 0, local_y = 0;
    int clip_right = SpriteFrame::kWidth - 1, clip_bottom = SpriteFrame::kHeight - 1;        // recorte do sistema
    int user_left = 0, user_top = 0, user_right = 0, user_bottom = 0;                         // recorte do usuário (comando 8)
    // Modo do comando atual (CMDPMOD): recorte de usuário, malha, MSB-on e cálculo de cor (bits 1-0).
    enum class UserClip { Off, Inside, Outside } user_clip = UserClip::Off;
    bool mesh = false, msb_on = false;
    int color_calc = 0;   // 0 substitui, 1 sombra, 2 meia-luminância, 3 meia-transparência
    bool simple = true;   // sem malha, recorte de usuário, MSB-on nem cálculo de cor: o desenho só substitui o ponto

    void set_mode(uint16_t pmod) {
        user_clip = !(pmod & 0x400) ? UserClip::Off : (pmod & 0x200) ? UserClip::Outside : UserClip::Inside;
        mesh = pmod & 0x100;
        msb_on = pmod & 0x8000;
        color_calc = pmod & 3;
        simple = !mesh && user_clip == UserClip::Off && !msb_on && color_calc == 0;
    }

    // Retângulo (em coordenadas do comando, antes da coordenada local) em que `plot` pode escrever.
    struct Bounds { int left, top, right, bottom; };
    Bounds bounds() const {
        return {-local_x, -local_y, std::min(clip_right, SpriteFrame::kWidth - 1) - local_x, std::min(clip_bottom, SpriteFrame::kHeight - 1) - local_y};
    }

    // Início da linha `y` (coordenada do comando) no frame buffer, deslocado pela coordenada local; só vale para x e y dentro de `bounds()`.
    uint16_t* row_ptr(int y) { return &out.data[size_t(y + local_y) * SpriteFrame::kWidth + local_x]; }

    void plot(int x, int y, uint16_t value) {
        x += local_x; y += local_y;
        if (x < 0 || y < 0 || x > clip_right || y > clip_bottom || x >= SpriteFrame::kWidth || y >= SpriteFrame::kHeight) return;
        if (simple) { out.data[size_t(y) * SpriteFrame::kWidth + x] = value; return; }
        if (mesh && ((x ^ y) & 1)) return;
        if (user_clip != UserClip::Off) {
            const bool inside = x >= user_left && x <= user_right && y >= user_top && y <= user_bottom;
            if (inside != (user_clip == UserClip::Inside)) return;
        }
        uint16_t& dest = out.data[size_t(y) * SpriteFrame::kWidth + x];
        if (msb_on) { dest |= 0x8000; return; }
        // Os modos de cálculo só valem para pixels RGB (MSB = 1); com paleta, o VDP1 apenas substitui.
        switch (color_calc) {
            case 1: if (dest & 0x8000) dest = halve(dest); break;                         // sombra: escurece o que já está lá
            case 2: dest = (value & 0x8000) ? halve(value) : value; break;
            case 3: dest = ((dest & 0x8000) && (value & 0x8000)) ? uint16_t(halve(dest) + (halve(value) & 0x7FFF)) : value; break;
            default: dest = value; break;
        }
    }
};

// Cor (valor de 16 bits do frame buffer) do texel (u, v) do sprite, ou -1 se transparente.
struct Texture {
    const Vram& vram;
    uint32_t base;
    int width, height, mode;
    uint16_t colr;
    bool draw_transparent;   // SPD: o código 0 também é desenhado
    uint32_t stride = uint32_t(mode <= 1 ? width / 2 : mode == 5 ? width * 2 : width);   // bytes por linha de textura

    int texel(int u, int v) const { return texel_in_row(base + uint32_t(v) * stride, u); }

    // Texel de coluna `u` na linha de textura que começa em `row` (endereço de VRAM).
    int texel_in_row(uint32_t row, int u) const {
        uint16_t value;
        switch (mode) {
            case 0: case 1: {
                const uint8_t byte = vram.u8(row + u / 2);
                const uint8_t nibble = (u & 1) ? (byte & 0xF) : (byte >> 4);
                if (nibble == 0 && !draw_transparent) return -1;
                value = mode == 0 ? uint16_t((colr & 0xFFF0) | nibble) : vram.u16(uint32_t(colr) * 8 + nibble * 2);
                break;
            }
            case 2: case 3: case 4: {
                const uint8_t byte = vram.u8(row + u);
                const uint16_t mask = mode == 2 ? 0x3F : mode == 3 ? 0x7F : 0xFF;
                if ((byte & mask) == 0 && !draw_transparent) return -1;
                value = uint16_t((colr & ~mask) | (byte & mask));
                break;
            }
            default:
                value = vram.u16(row + uint32_t(u) * 2);
                break;
        }
        return value ? value : (draw_transparent ? 0x0000 : -1);
    }
};

void draw_line(Context& c, Point a, Point b, uint16_t color) {
    int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        c.plot(a.x, a.y, color);
        if (a.x == b.x && a.y == b.y) break;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; a.x += sx; }
        if (e2 < dx) { err += dx; a.y += sy; }
    }
}

// Sombreamento de Gouraud: cada canto traz uma cor RGB555 cujo desvio em relação a 16 (neutro) é somado, por canal, ao pixel RGB.
struct Gouraud {
    bool enabled = false;
    uint16_t corner[4] = {};   // A, B, C, D

    static int channel(uint16_t color, int shift) { return (color >> shift) & 31; }
    uint16_t apply(uint16_t pixel, double fx, double fy) const {
        if (!enabled || !(pixel & 0x8000)) return pixel;
        uint16_t out = pixel & 0x8000;
        for (int shift = 0; shift <= 10; shift += 5) {
            const double top = channel(corner[0], shift) + (channel(corner[1], shift) - channel(corner[0], shift)) * fx;
            const double bottom = channel(corner[3], shift) + (channel(corner[2], shift) - channel(corner[3], shift)) * fx;
            const double offset = top + (bottom - top) * fy - 16.0;
            const int value = std::clamp(int(std::lround(channel(pixel, shift) + offset)), 0, 31);
            out = uint16_t(out | (value << shift));
        }
        return out;
    }
};

// Percorre o quadrilátero A(topo-esq) B(topo-dir) C(base-dir) D(base-esq) linha a linha entre as arestas A-D e B-C, como o VDP1, e
// chama `emit(x, y, fx, fy)` para cada ponto com as frações horizontal e vertical da textura.
// Arredondamento para o inteiro mais próximo, empates para longe do zero (igual a std::lround, sem a chamada de biblioteca).
inline int round_half_away(double v) {
    const int t = int(v);   // trunca em direção a zero; a diferença v - t é exata em ponto flutuante
    const double frac = v - t;
    return t + int(frac >= 0.5) - int(frac <= -0.5);   // sem desvios: o arredondamento é imprevisível para o preditor
}

// `row(fy)` é chamado ao começar cada linha que tem alguma parte visível.
template <typename Row, typename Emit>
void walk_quad(Point a, Point b, Point cc, Point d, const Context::Bounds& bounds, Row&& row, Emit&& emit) {
    const auto length = [](Point p, Point q) { return std::max(std::abs(p.x - q.x), std::abs(p.y - q.y)); };
    const int rows = std::max(length(a, d), length(b, cc)) + 1;
    const double row_div = rows > 1 ? double(rows - 1) : 1.0;
    for (int r = 0; r < rows; ++r) {
        const double fy = rows > 1 ? double(r) / row_div : 0.0;
        const double lx = a.x + (d.x - a.x) * fy, ly = a.y + (d.y - a.y) * fy;
        const double rx = b.x + (cc.x - b.x) * fy, ry = b.y + (cc.y - b.y) * fy;
        const int rlx = round_half_away(lx), rrx = round_half_away(rx), rly = round_half_away(ly), rry = round_half_away(ry);
        const int cols = std::max(std::abs(rrx - rlx), std::abs(rry - rly)) + 1;
        // Linha inteira fora da área visível (com 1 ponto de folga): nenhum de seus pontos seria desenhado.
        if (std::max(rlx, rrx) < bounds.left - 1 || std::min(rlx, rrx) > bounds.right + 1 ||
            std::max(rly, rry) < bounds.top - 1 || std::min(rly, rry) > bounds.bottom + 1) continue;
        const double dx = rx - lx, dy = ry - ly, col_div = cols > 1 ? double(cols - 1) : 1.0;
        // Faixa de s em que o ponto pode cair dentro da área visível: cada eixo é monotônico em s, então basta achar onde a reta
        // entra e sai do retângulo (com 2 pontos de folga); o teste exato continua sendo feito por ponto.
        int s_begin = 0, s_end = cols;
        if (cols > 1) {
            const auto narrow = [&](double origin, double delta, int lo, int hi) {
                if (std::abs(delta) < 1e-9) return;   // eixo constante: já foi verificado pelo teste da linha inteira
                double f1 = (lo - 2 - origin) / delta, f2 = (hi + 2 - origin) / delta;
                if (f1 > f2) std::swap(f1, f2);
                const double b = std::floor(f1 * col_div) - 1, e = std::ceil(f2 * col_div) + 2;
                s_begin = std::max(s_begin, int(std::clamp(b, 0.0, double(cols))));
                s_end = std::min(s_end, int(std::clamp(e, 0.0, double(cols))));
            };
            narrow(lx, dx, bounds.left, bounds.right);
            narrow(ly, dy, bounds.top, bounds.bottom);
        }
        row(fy);
        for (int s = s_begin; s < s_end; ++s) {
            const double fx = cols > 1 ? double(s) / col_div : 0.0;
            const int x = round_half_away(lx + dx * fx), y = round_half_away(ly + dy * fx);
            if (x < bounds.left || x > bounds.right || y < bounds.top || y > bounds.bottom) continue;   // plot() descartaria
            emit(x, y, fx, fy);
        }
    }
}

// Quadrilátero alinhado aos eixos (A-B horizontal, A-D vertical, sem inclinação): é o caso dos sprites normais/escalados e da maioria
// dos distorcidos. As contas de ponto flutuante de `walk_quad` se reduzem a x = A.x ± s (sem empates) e y = round(ly) constante por
// linha, de modo que o resultado é o mesmo, com a divisão por ponto feita uma vez só (tabela) em vez de por pixel.
bool axis_aligned(Point a, Point b, Point cc, Point d) { return a.y == b.y && d.y == cc.y && a.x == d.x && b.x == cc.x; }

// Percorre as linhas visíveis; `row(y, fy)` é chamado por linha e devolve verdadeiro para continuar. `span` descreve os pontos s visíveis.
struct AxisSpan { int s_lo, s_hi, x0, step, last; };   // x(s) = x0 + step * s; `last` = número de colunas - 1

AxisSpan axis_span(Point a, Point b, const Context::Bounds& bounds) {
    const int width = b.x - a.x, last = std::abs(width), step = (width > 0) - (width < 0);
    AxisSpan sp{0, last, a.x, step, last};
    if (step == 0) { if (a.x < bounds.left || a.x > bounds.right) sp.s_hi = -1; }
    else if (step > 0) { sp.s_lo = std::max(0, bounds.left - a.x); sp.s_hi = std::min(last, bounds.right - a.x); }
    else { sp.s_lo = std::max(0, a.x - bounds.right); sp.s_hi = std::min(last, a.x - bounds.left); }
    return sp;
}

template <typename RowFn>
void walk_axis_rows(Point a, Point d, const Context::Bounds& bounds, RowFn&& row) {
    const int rows = std::abs(d.y - a.y) + 1;
    const double row_div = rows > 1 ? double(rows - 1) : 1.0;
    for (int r = 0; r < rows; ++r) {
        const double fy = rows > 1 ? double(r) / row_div : 0.0;
        const int y = round_half_away(a.y + (d.y - a.y) * fy);
        if (y < bounds.top || y > bounds.bottom) continue;
        row(y, fy);
    }
}

void draw_textured_quad(Context& c, const Texture& t, const Gouraud& g, Point a, Point b, Point cc, Point d, bool hflip, bool vflip) {
    const Context::Bounds bounds = c.bounds();
    if (!g.enabled && axis_aligned(a, b, cc, d)) {
        const AxisSpan sp = axis_span(a, b, bounds);
        if (sp.s_hi < sp.s_lo) return;
        std::array<int, SpriteFrame::kWidth> u_of;   // u de cada s visível (a faixa visível nunca passa da largura do frame buffer)
        for (int s = sp.s_lo; s <= sp.s_hi; ++s) {
            const double fx = sp.last > 0 ? double(s) / double(sp.last) : 0.0;
            const int u = std::min(t.width - 1, int(fx * t.width));
            u_of[s - sp.s_lo] = hflip ? t.width - 1 - u : u;
        }
        walk_axis_rows(a, d, bounds, [&](int y, double fy) {
            int v = std::min(t.height - 1, int(fy * t.height));
            if (vflip) v = t.height - 1 - v;
            const uint32_t row_addr = t.base + uint32_t(v) * t.stride;
            if (c.simple) {   // só substituir o ponto: grava direto na linha, sem repetir os testes de recorte (já feitos pela faixa visível)
                uint16_t* dst = c.row_ptr(y);
                for (int s = sp.s_lo; s <= sp.s_hi; ++s) {
                    const int texel = t.texel_in_row(row_addr, u_of[s - sp.s_lo]);
                    if (texel >= 0) dst[sp.x0 + sp.step * s] = uint16_t(texel);
                }
                return;
            }
            for (int s = sp.s_lo; s <= sp.s_hi; ++s) {
                const int texel = t.texel_in_row(row_addr, u_of[s - sp.s_lo]);
                if (texel >= 0) c.plot(sp.x0 + sp.step * s, y, uint16_t(texel));
            }
        });
        return;
    }
    int v = 0;
    uint32_t row_addr = 0;
    walk_quad(a, b, cc, d, bounds, [&](double fy) {
        v = std::min(t.height - 1, int(fy * t.height));
        if (vflip) v = t.height - 1 - v;
        row_addr = t.base + uint32_t(v) * t.stride;
    }, [&](int x, int y, double fx, double fy) {
        int u = std::min(t.width - 1, int(fx * t.width));
        if (hflip) u = t.width - 1 - u;
        const int texel = t.texel_in_row(row_addr, u);
        if (texel >= 0) c.plot(x, y, g.apply(uint16_t(texel), fx, fy));
    });
}

void draw_polygon(Context& c, const Gouraud& g, uint16_t color, Point a, Point b, Point cc, Point d) {
    const Context::Bounds bounds = c.bounds();
    if (!g.enabled && axis_aligned(a, b, cc, d)) {
        const AxisSpan sp = axis_span(a, b, bounds);
        if (sp.s_hi < sp.s_lo) return;
        walk_axis_rows(a, d, bounds, [&](int y, double) {
            if (c.simple) {
                uint16_t* dst = c.row_ptr(y);
                for (int s = sp.s_lo; s <= sp.s_hi; ++s) dst[sp.x0 + sp.step * s] = color;
                return;
            }
            for (int s = sp.s_lo; s <= sp.s_hi; ++s) c.plot(sp.x0 + sp.step * s, y, color);
        });
        return;
    }
    walk_quad(a, b, cc, d, bounds, [](double) {}, [&](int x, int y, double fx, double fy) { c.plot(x, y, g.apply(color, fx, fy)); });
}

}  // namespace

SpriteFrame render_vdp1(std::span<const uint8_t> vram_bytes) {
    SpriteFrame out;
    const Vram vram{vram_bytes};
    Context c{out};
    uint32_t address = 0;
    std::vector<uint32_t> return_stack;
    for (int executed = 0; executed < kMaxCommands; ++executed) {
        const uint16_t ctrl = vram.u16(address), link = vram.u16(address + 2), pmod = vram.u16(address + 4), colr = vram.u16(address + 6);
        const uint16_t srca = vram.u16(address + 8), size = vram.u16(address + 10);
        const auto coord = [&](uint32_t off) { return int(int16_t(vram.u16(address + off))); };
        const Point pa{coord(0x0C), coord(0x0E)}, pb{coord(0x10), coord(0x12)}, pc{coord(0x14), coord(0x16)}, pd{coord(0x18), coord(0x1A)};
        if (ctrl & 0x8000) break;   // END

        if (!(ctrl & 0x4000)) {     // SKIP desligado: executa
            const int command = ctrl & 0xF;
            const bool hflip = ctrl & 0x10, vflip = ctrl & 0x20;
            const Texture tex{vram, uint32_t(srca) * 8, ((size >> 8) & 0x3F) * 8, size & 0xFF, (pmod >> 3) & 7, colr, bool(pmod & 0x40)};
            c.set_mode(pmod);
            Gouraud gouraud;
            if (pmod & 4) {   // sombreamento de Gouraud: tabela de 4 cores no endereço GRDA * 8
                gouraud.enabled = true;
                const uint32_t table = uint32_t(vram.u16(address + 0x1C)) * 8;
                for (int i = 0; i < 4; ++i) gouraud.corner[i] = vram.u16(table + 2 * i);
            }
            switch (command) {
                case 0:   // sprite normal
                    draw_textured_quad(c, tex, gouraud, pa, {pa.x + tex.width - 1, pa.y}, {pa.x + tex.width - 1, pa.y + tex.height - 1}, {pa.x, pa.y + tex.height - 1}, hflip, vflip);
                    break;
                case 1: { // sprite escalado
                    const int zp = (ctrl >> 8) & 0xF;
                    Point tl = pa, br = pc;
                    if (zp != 0) {
                        const int w = pb.x, h = pb.y;
                        const int col = zp & 3, row = zp >> 2;   // 1..3: esquerda/centro/direita; 1..3: topo/meio/base
                        tl.x = col == 1 ? pa.x : col == 2 ? pa.x - w / 2 : pa.x - w;
                        tl.y = row == 1 ? pa.y : row == 2 ? pa.y - h / 2 : pa.y - h;
                        br = {tl.x + w, tl.y + h};
                    }
                    draw_textured_quad(c, tex, gouraud, tl, {br.x, tl.y}, br, {tl.x, br.y}, hflip, vflip);
                    break;
                }
                case 2: draw_textured_quad(c, tex, gouraud, pa, pb, pc, pd, hflip, vflip); break;
                case 4: draw_polygon(c, gouraud, colr, pa, pb, pc, pd); break;
                case 5: draw_line(c, pa, pb, colr); draw_line(c, pb, pc, colr); draw_line(c, pc, pd, colr); draw_line(c, pd, pa, colr); break;
                case 6: draw_line(c, pa, pb, colr); break;
                case 8: c.user_left = pa.x; c.user_top = pa.y; c.user_right = pc.x; c.user_bottom = pc.y; break;
                case 9: c.clip_right = pc.x; c.clip_bottom = pc.y; break;
                case 10: c.local_x = pa.x; c.local_y = pa.y; break;
                default: break;   // desconhecidos: ignorados
            }
        }

        switch ((ctrl >> 12) & 3) {   // JP: 0 próximo, 1 salto, 2 chamada, 3 retorno
            case 0: address += kCommandBytes; break;
            case 1: address = uint32_t(link) * 8; break;
            case 2: return_stack.push_back(address + kCommandBytes); address = uint32_t(link) * 8; break;
            default:
                if (return_stack.empty()) return out;
                address = return_stack.back();
                return_stack.pop_back();
        }
    }
    return out;
}

}  // namespace video
