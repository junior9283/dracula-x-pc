#include "video/vdp2.hpp"

#include <algorithm>
#include <array>

namespace video {
namespace {

constexpr uint32_t kTvmd = 0x00, kRamctl = 0x0E, kBgon = 0x20, kChctla = 0x28, kChctlb = 0x2A, kPncn0 = 0x30, kPlsz = 0x3A, kBmpna = 0x2C, kZmctl = 0x98, kScrctl = 0x9A, kLsta0 = 0xA0, kVcsta = 0x9C, kZoom0 = 0x78, kMpofn = 0x3C, kMpabn0 = 0x40;
constexpr uint32_t kScroll0 = 0x70, kScroll1 = 0x80, kScrollN2 = 0x90, kScrollN3 = 0x94, kPrina = 0xF8, kPrinb = 0xFA, kBktau = 0xAC, kSpctl = 0xE0, kPrisa = 0xF0;
constexpr uint32_t kWindowPos = 0xC0, kWctla = 0xD0, kCraofa = 0xE4, kCraofb = 0xE6, kSfprmd = 0xEA, kCcctl = 0xEC, kSfccmd = 0xEE;
constexpr uint32_t kPncr = 0x38, kMpofr = 0x3E, kMpabra = 0x50, kRpmd = 0xB0, kKtctl = 0xB4, kKtaof = 0xB6, kOvpnr = 0xB8, kRptau = 0xBC, kLwta0 = 0xD8, kPrir = 0xFC, kCcrr = 0x10C, kBmpnb = 0x2E;
constexpr uint32_t kCcrsa = 0x100, kCcrna = 0x108, kClofen = 0x110, kClofsl = 0x112, kCoa = 0x114, kCob = 0x11A;

struct Regs {
    std::span<const uint8_t> r;
    uint16_t u16(uint32_t off) const { return off + 1 < r.size() ? uint16_t((r[off] << 8) | r[off + 1]) : 0; }
};

uint16_t vram16(std::span<const uint8_t> vram, uint32_t addr) {
    addr &= uint32_t(vram.size()) - 1;
    return uint16_t((vram[addr] << 8) | vram[addr + 1]);
}
uint8_t vram8(std::span<const uint8_t> vram, uint32_t addr) { return vram[addr & (uint32_t(vram.size()) - 1)]; }

uint32_t rgb555(uint16_t c) {
    const auto expand = [](uint32_t v) { return uint8_t((v << 3) | (v >> 2)); };
    return rgba(expand(c & 31), expand((c >> 5) & 31), expand((c >> 10) & 31));
}

// Cor da RAM de cores pelo índice, segundo o modo de RAMCTL (0: RGB555 1024 cores, 1: RGB555 2048, 2: RGB888 1024).
uint32_t cram_color(std::span<const uint8_t> cram, int mode, uint32_t index) {
    if (mode == 2) {
        const uint32_t addr = (index & 0x3FF) * 4;
        return addr + 3 < cram.size() ? rgba(cram[addr + 3], cram[addr + 2], cram[addr + 1]) : 0;
    }
    const uint32_t addr = (index & (mode == 0 ? 0x3FF : 0x7FF)) * 2;
    return rgb555(uint16_t((cram[addr % cram.size()] << 8) | cram[(addr + 1) % cram.size()]));
}

struct Nbg {
    bool enabled = false, transparent_code_visible = false;
    int char_size = 0;          // 0: 8x8, 1: 16x16
    bool bitmap = false;
    int color_mode = 0;         // CHCN: 0 = 16, 1 = 256, 2 = 2048, 3 = 32k, 4 = 16M
    int priority = 0;
    bool one_word = false, supplement_mode = false, supplement_spr = false, supplement_scc = false;
    uint32_t supplement_char = 0, supplement_palette = 0;
    int plane_width = 1, plane_height = 1;   // páginas por plano
    std::array<uint32_t, 4> map{};           // número de página inicial de cada plano (A, B, C, D)
    int scroll_x = 0, scroll_y = 0;          // rolagem em ponto fixo 11.8 (parte inteira << 8 | fração)
    int zoom_x = 256, zoom_y = 256, zoom_x_max = 256;          // incremento de coordenada por ponto de tela em 1/256 (256 = sem zoom)
    // Rolagem por linha (SCRCTL) e vertical por célula; as tabelas ficam na VRAM, entradas de 32 bits (11.8 fixo nos bits 26-8).
    bool line_x = false, line_y = false, line_zoom = false, vcell = false;
    int line_shift = 0;                      // log2 do intervalo de linhas por entrada
    uint32_t line_addr = 0, vcell_addr = 0, vcell_stride = 4, vcell_offset = 0;
    int bmp_width = 512, bmp_height = 256;   // bitmap
    uint32_t bmp_addr = 0, bmp_palette = 0;  // BMPNA: bits 6-4 do número de paleta (já como índice base << 8)
    bool bmp_special_priority = false, bmp_special_cc = false;
    uint32_t cram_offset = 0;                // CRAOFA: somado ao índice de paleta
    int special_priority = 0;                // SFPRMD: 0 por tela, 1 por caractere (bit SPR), 2 por ponto (tratado como 0)
    bool cc_enabled = false, cc_per_char = false;
    int cc_ratio = 0;
    uint8_t window = 0;                      // byte de WCTLx desta camada
    // Camada de rotação (RBG0/RBG1): `rot_param` = 0/1 fixa o parâmetro A/B (RBG1 é sempre B); `rot_map` são os 16 planos.
    bool rotating = false;
    int rot_param = 0, rot_over = 0;         // rot_over: modo de estouro da área (RAOVR/RBOVR): 0 repete, 1 padrão fixo, 2 e 3 transparente
    uint16_t rot_over_pn = 0;
    std::array<uint32_t, 16> rot_map{};      // página inicial de cada plano (A..P)
};

constexpr uint32_t kNoPixel = 0;   // alfa 0 = transparente

// LUT da RAM de cores já convertida em RGBA para o modo de RAMCTL do quadro; o índice é mascarado em 11 bits.
using ColorLut = std::array<uint32_t, 2048>;
uint32_t lut_color(const ColorLut& lut, uint32_t index) { return lut[index & 0x7FF]; }

// Cor da célula (um pixel de uma letra) na profundidade `Mode` (CHCN); 0 = transparente.
template <int Mode>
inline uint32_t cell_pixel_m(const Vdp2State& s, const Nbg& n, const ColorLut& lut, uint32_t char_addr, int px, int py, uint32_t palette) {
    uint32_t code;
    if constexpr (Mode == 0) {
        code = vram8(s.vram, char_addr + py * 4 + px / 2);
        code = (px & 1) ? (code & 0xF) : (code >> 4);
        return (code != 0 || n.transparent_code_visible) ? lut_color(lut, n.cram_offset + palette * 16 + code) : kNoPixel;   // sem desvio (cmov)
    } else if constexpr (Mode == 1) {
        code = vram8(s.vram, char_addr + py * 8 + px);
        return (code != 0 || n.transparent_code_visible) ? lut_color(lut, n.cram_offset + (palette & 7) * 256 + code) : kNoPixel;
    } else if constexpr (Mode == 2) {
        code = vram16(s.vram, char_addr + (py * 8 + px) * 2) & 0x7FF;
        if (code == 0 && !n.transparent_code_visible) return kNoPixel;
        return lut_color(lut, n.cram_offset + code);
    } else if constexpr (Mode == 3) {
        const uint16_t c = vram16(s.vram, char_addr + (py * 8 + px) * 2);
        if (!(c & 0x8000) && !n.transparent_code_visible) return kNoPixel;
        return rgb555(c);
    } else {
        const uint32_t a = char_addr + (py * 8 + px) * 4;
        const uint32_t v = (uint32_t(vram16(s.vram, a)) << 16) | vram16(s.vram, a + 2);
        if (!(v & 0x80000000u) && !n.transparent_code_visible) return kNoPixel;
        return rgba(uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16));
    }
}

uint32_t cell_pixel(const Vdp2State& s, const Nbg& n, const ColorLut& lut, uint32_t char_addr, int px, int py, uint32_t palette) {
    switch (n.color_mode) {
        case 0: return cell_pixel_m<0>(s, n, lut, char_addr, px, py, palette);
        case 1: return cell_pixel_m<1>(s, n, lut, char_addr, px, py, palette);
        case 2: return cell_pixel_m<2>(s, n, lut, char_addr, px, py, palette);
        case 3: return cell_pixel_m<3>(s, n, lut, char_addr, px, py, palette);
        default: return cell_pixel_m<4>(s, n, lut, char_addr, px, py, palette);
    }
}

// Ponto (x, y) do bitmap de NBG0/NBG1 (já dentro das dimensões); 0 = transparente.
uint32_t bitmap_pixel(const Vdp2State& s, const Nbg& n, const ColorLut& lut, int x, int y) {
    const uint32_t offset = uint32_t(y) * uint32_t(n.bmp_width) + uint32_t(x);
    uint32_t code;
    switch (n.color_mode) {
        case 0:
            code = vram8(s.vram, n.bmp_addr + offset / 2);
            code = (offset & 1) ? (code & 0xF) : (code >> 4);
            if (code == 0 && !n.transparent_code_visible) return kNoPixel;
            return lut_color(lut, n.cram_offset + n.bmp_palette + code);
        case 1:
            code = vram8(s.vram, n.bmp_addr + offset);
            if (code == 0 && !n.transparent_code_visible) return kNoPixel;
            return lut_color(lut, n.cram_offset + n.bmp_palette + code);
        case 2:
            code = vram16(s.vram, n.bmp_addr + offset * 2) & 0x7FF;
            if (code == 0 && !n.transparent_code_visible) return kNoPixel;
            return lut_color(lut, n.cram_offset + code);
        case 3: {
            const uint16_t c = vram16(s.vram, n.bmp_addr + offset * 2);
            if (!(c & 0x8000) && !n.transparent_code_visible) return kNoPixel;
            return rgb555(c);
        }
        default: {
            const uint32_t a = n.bmp_addr + offset * 4;
            const uint32_t v = (uint32_t(vram16(s.vram, a)) << 16) | vram16(s.vram, a + 2);
            if (!(v & 0x80000000u) && !n.transparent_code_visible) return kNoPixel;
            return rgba(uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16));
        }
    }
}

int cell_bytes(int color_mode) { return color_mode == 0 ? 32 : color_mode == 1 ? 64 : color_mode == 4 ? 256 : 128; }

// Letra resolvida pelo pattern name de uma célula: endereço, paleta, espelhamentos e bits especiais.
struct Cell { uint32_t char_base = 0, palette = 0; bool hflip = false, vflip = false, spr = false, scc = false; };

// Endereços (em bytes) do início da linha de pattern names de cada trecho de 512 pontos de uma linha do plano; `wy` fixo.
// O índice é x >> 9 (0..3): com plano de 2 páginas de largura, os trechos 2 e 3 pertencem ao plano seguinte.
using RowMap = std::array<uint32_t, 4>;

RowMap make_row_map(const Nbg& n, int wy) {
    const int cell_shift = n.char_size ? 4 : 3;
    const int page_cells = n.char_size ? 32 : 64;          // patterns por lado de uma página (cada página tem 512 pontos de lado)
    const int plane_shift_y = 9 + (n.plane_height - 1);
    const uint32_t page_bytes = uint32_t(page_cells) * page_cells * (n.one_word ? 2 : 4);
    const uint32_t row_in_page = uint32_t((wy & 511) >> cell_shift) * page_cells * (n.one_word ? 2 : 4);
    const int plane_row = wy >> plane_shift_y, page_row = (wy >> 9) & (n.plane_height - 1);
    RowMap map{};
    for (int i = 0; i < 4; ++i) {
        const int plane_col = i >> (n.plane_width - 1), page_col = i & (n.plane_width - 1);
        if (plane_col > 1) continue;
        const uint32_t plane_base_page = n.map[plane_row * 2 + plane_col];
        map[i] = (plane_base_page + page_row * n.plane_width + page_col) * page_bytes + row_in_page;
    }
    return map;
}

// Decodifica um pattern name de 1 palavra (com os bits suplementares da camada).
Cell decode_one_word(const Nbg& n, uint16_t pn) {
    Cell c;
    c.palette = ((pn >> 12) & 0xF) | (n.supplement_palette << 4);
    c.spr = n.supplement_spr;
    c.scc = n.supplement_scc;
    c.vflip = !n.supplement_mode && (pn & 0x800);
    c.hflip = !n.supplement_mode && (pn & 0x400);
    const uint32_t sc = n.supplement_char;
    uint32_t char_number;
    if (n.char_size) {
        const uint32_t pn_bits = n.supplement_mode ? (pn & 0xFFF) : (pn & 0x3FF);
        const uint32_t high = n.supplement_mode ? ((sc & 0x10) << 10) : ((sc & 0x1C) << 10);
        char_number = high | (pn_bits << 2) | (sc & 3);
    } else {
        char_number = n.supplement_mode ? (((sc & 0x1C) << 10) | (pn & 0xFFF)) : ((sc << 10) | (pn & 0x3FF));
    }
    c.char_base = char_number * 0x20;
    return c;
}

// Decodifica um pattern name de 2 palavras.
Cell decode_two_words(const Nbg& n, uint16_t w0, uint16_t w1) {
    Cell c;
    // Número de paleta de 7 bits; em 256 cores só contam os bits 6-4 (no formato de 1 palavra eles já chegam em 2-0).
    c.palette = n.color_mode == 1 ? (w0 >> 4) & 7 : w0 & 0x7F;
    c.vflip = w0 & 0x8000;
    c.hflip = w0 & 0x4000;
    c.spr = w0 & 0x2000;
    c.scc = w0 & 0x1000;
    c.char_base = uint32_t(w1 & 0x7FFF) * 0x20;
    return c;
}

// Resolve o pattern name da célula que contém a coluna `x` (já dentro de 0..total-1). Todos os tamanhos são potências de 2.
Cell resolve_cell(const Vdp2State& s, const Nbg& n, const RowMap& row_map, int x) {
    const int cell_shift = n.char_size ? 4 : 3;
    const uint32_t entry_bytes = n.one_word ? 2 : 4;
    const uint32_t page_addr = row_map[x >> 9];
    const uint32_t entry = uint32_t((x & 511) >> cell_shift) * entry_bytes;
    if (n.one_word) return decode_one_word(n, vram16(s.vram, page_addr + entry));
    return decode_two_words(n, vram16(s.vram, page_addr + entry), vram16(s.vram, page_addr + entry + 2));
}

uint32_t vram32(std::span<const uint8_t> vram, uint32_t addr) { return (uint32_t(vram16(vram, addr)) << 16) | vram16(vram, addr + 2); }
int fixed_from_table(uint32_t v) { return int((v >> 8) & 0x7FFFF); }   // 11.8 em ponto fixo (bits 26-8)

// Cor do ponto (px, py) (já dentro da letra, antes dos espelhamentos) de uma letra resolvida; 0 = transparente.
uint32_t cell_color(const Vdp2State& s, const Nbg& n, const ColorLut& lut, const Cell& cell, int px, int py) {
    const int cell_mask = (n.char_size ? 16 : 8) - 1;
    if (cell.hflip) px = cell_mask - px;
    if (cell.vflip) py = cell_mask - py;
    // Letras 16x16 são 4 células 8x8 consecutivas (esquerda-cima, direita-cima, esquerda-baixo, direita-baixo).
    uint32_t char_addr = cell.char_base;
    if (n.char_size) {
        char_addr += uint32_t((py >> 3) * 2 + (px >> 3)) * cell_bytes(n.color_mode);
        px &= 7; py &= 7;
    }
    return cell_pixel(s, n, lut, char_addr, px, py, cell.palette);
}

// Preenche `count` pontos consecutivos de uma mesma linha de célula, a partir da coluna `in_cell` da célula. Os parâmetros ficam em
// variáveis locais para que as gravações em `color` (uint32_t) não obriguem o compilador a reler os campos de `n` a cada ponto.
template <int Mode>
void fill_run(const Vdp2State& s, const Nbg& n, const ColorLut& lut, const Cell& cell, int in_cell, int py, int count, uint32_t* color, uint8_t* flags) {
    const int cell_mask = (n.char_size ? 16 : 8) - 1;
    const bool sixteen = n.char_size, hflip = cell.hflip, visible0 = n.transparent_code_visible;
    const uint32_t cram_offset = n.cram_offset, palette = cell.palette;
    const uint8_t* const vram = s.vram.data();
    const uint32_t mask = uint32_t(s.vram.size()) - 1;
    const uint32_t* const colors = lut.data();
    const uint8_t cell_flags = uint8_t(cell.spr) | (cell.scc << 1);
    if (cell.vflip) py = cell_mask - py;
    const int row = py & 7;
    const uint32_t row_base = cell.char_base + (sixteen ? uint32_t(py >> 3) * 2 * cell_bytes(Mode) : 0);
    if constexpr (Mode == 0) {
        if (!sixteen) {   // 8x8 de 16 cores: a linha da célula são 4 bytes, lidos uma vez só (o pixel 0 fica nos 4 bits altos)
            const uint32_t a = cell.char_base + row * 4;
            const uint32_t word = (uint32_t(vram[a & mask]) << 24) | (uint32_t(vram[(a + 1) & mask]) << 16) | (uint32_t(vram[(a + 2) & mask]) << 8) | vram[(a + 3) & mask];
            const uint32_t base = cram_offset + palette * 16;
            for (int k = 0; k < count; ++k) {
                const int px = hflip ? 7 - (in_cell + k) : in_cell + k;
                const uint32_t code = (word >> (28 - 4 * px)) & 0xF;
                color[k] = (code != 0 || visible0) ? colors[(base + code) & 0x7FF] : kNoPixel;
                flags[k] = cell_flags;
            }
            return;
        }
    }
    for (int k = 0; k < count; ++k) {
        int px = in_cell + k;
        if (hflip) px = cell_mask - px;
        const uint32_t char_addr = sixteen ? row_base + uint32_t(px >> 3) * cell_bytes(Mode) : row_base;
        const int col = px & 7;
        uint32_t out;
        if constexpr (Mode == 0) {
            const uint32_t byte = vram[(char_addr + row * 4 + (col >> 1)) & mask];
            const uint32_t code = (col & 1) ? (byte & 0xF) : (byte >> 4);
            out = (code != 0 || visible0) ? colors[(cram_offset + palette * 16 + code) & 0x7FF] : kNoPixel;
        } else if constexpr (Mode == 1) {
            const uint32_t code = vram[(char_addr + row * 8 + col) & mask];
            out = (code != 0 || visible0) ? colors[(cram_offset + (palette & 7) * 256 + code) & 0x7FF] : kNoPixel;
        } else {
            out = cell_pixel_m<Mode>(s, n, lut, char_addr, col, row, palette);
        }
        color[k] = out;
        flags[k] = cell_flags;
    }
}

// Preenche uma linha da camada NBG (cor RGBA, 0 = transparente; bit 0 de `flags` = prioridade especial, bit 1 = cálculo de cor especial),
// com rolagem fracionária, zoom, rolagem por linha e vertical por célula.
void fill_row(const Vdp2State& s, const Nbg& n, const ColorLut& lut, int y, int width, uint32_t* color, uint8_t* flags) {
    int sx = n.scroll_x, sy = n.scroll_y, xstep = n.zoom_x;
    if (n.line_x || n.line_y || n.line_zoom) {
        const int stride = 4 * (int(n.line_x) + int(n.line_y) + int(n.line_zoom));
        uint32_t addr = n.line_addr + uint32_t(y >> n.line_shift) * stride;
        if (n.line_x) { sx += fixed_from_table(vram32(s.vram, addr)); addr += 4; }
        if (n.line_y) { sy += fixed_from_table(vram32(s.vram, addr)); addr += 4; }
        if (n.line_zoom) {
            const int v = int((vram32(s.vram, addr) >> 8) & 0x7FF);
            xstep = v == 0 ? 256 : std::min(v, n.zoom_x_max);
        }
    }
    const int base_y = sy + y * n.zoom_y;
    const int width_mask = n.bitmap ? n.bmp_width - 1 : (1024 << (n.plane_width - 1)) - 1;
    const int height_mask = n.bitmap ? n.bmp_height - 1 : (1024 << (n.plane_height - 1)) - 1;
    const int cell_shift = n.char_size ? 4 : 3, cell_mask = (1 << cell_shift) - 1;
    const int vcell_phase = (sx >> 8) & 7;

    if (!n.bitmap && !n.vcell && xstep == 256) {   // sem zoom horizontal nem rolagem por célula: uma célula por vez, sem recalcular o pattern name por ponto
        const int wy = (base_y >> 8) & height_mask, wx0 = (sx >> 8);
        const RowMap row_map = make_row_map(n, wy);
        for (int x = 0; x < width;) {
            const int wx = (wx0 + x) & width_mask, in_cell = wx & cell_mask;
            const int count = std::min(width - x, (cell_mask + 1) - in_cell);
            const Cell cell = resolve_cell(s, n, row_map, wx);
            const int py = wy & cell_mask;
            switch (n.color_mode) {
                case 0: fill_run<0>(s, n, lut, cell, in_cell, py, count, color + x, flags + x); break;
                case 1: fill_run<1>(s, n, lut, cell, in_cell, py, count, color + x, flags + x); break;
                case 2: fill_run<2>(s, n, lut, cell, in_cell, py, count, color + x, flags + x); break;
                case 3: fill_run<3>(s, n, lut, cell, in_cell, py, count, color + x, flags + x); break;
                default: fill_run<4>(s, n, lut, cell, in_cell, py, count, color + x, flags + x); break;
            }
            x += count;
        }
        return;
    }
    int last_cell = -1, last_column = -1, wy = 0;
    Cell cell;
    RowMap row_map{};
    for (int x = 0; x < width; ++x) {
        const int wx = ((sx + x * xstep) >> 8) & width_mask;
        const int column = n.vcell ? (x + vcell_phase) >> 3 : 0;
        if (column != last_column) {
            int row_fp = base_y;
            if (n.vcell) row_fp += fixed_from_table(vram32(s.vram, n.vcell_addr + uint32_t(column) * n.vcell_stride + n.vcell_offset));
            wy = (row_fp >> 8) & height_mask;
            if (!n.bitmap) row_map = make_row_map(n, wy);
            last_column = column;
            last_cell = -1;
        }
        if (n.bitmap) {
            color[x] = bitmap_pixel(s, n, lut, wx, wy);
            flags[x] = uint8_t(n.bmp_special_priority) | (n.bmp_special_cc << 1);
            continue;
        }
        const int cell_index = wx >> cell_shift;
        if (cell_index != last_cell) { cell = resolve_cell(s, n, row_map, wx); last_cell = cell_index; }
        color[x] = cell_color(s, n, lut, cell, wx & cell_mask, wy & cell_mask);
        flags[x] = uint8_t(cell.spr) | (cell.scc << 1);
    }
}

// Registrador de pattern name (PNCNx/PNCR): formato do pattern name e bits suplementares.
void apply_pnc(Nbg& n, uint16_t pncn) {
    n.one_word = pncn & 0x8000;
    n.supplement_mode = pncn & 0x4000;
    n.supplement_palette = (pncn >> 5) & 7;
    n.supplement_char = pncn & 0x1F;
    n.supplement_spr = pncn & 0x200;
    n.supplement_scc = pncn & 0x100;
}

Nbg read_nbg(const Regs& g, int index) {
    Nbg n;
    const uint16_t bgon = g.u16(kBgon), chctla = g.u16(kChctla), chctlb = g.u16(kChctlb);
    n.enabled = bgon & (1 << index);
    n.transparent_code_visible = bgon & (0x100 << index);
    int bitmap_size = 0;
    switch (index) {
        case 0: n.char_size = chctla & 1; n.bitmap = chctla & 2; n.color_mode = (chctla >> 4) & 7; bitmap_size = (chctla >> 2) & 3; break;
        case 1: n.char_size = (chctla >> 8) & 1; n.bitmap = chctla & 0x200; n.color_mode = (chctla >> 12) & 3; bitmap_size = (chctla >> 10) & 3; break;
        case 2: n.char_size = chctlb & 1; n.bitmap = false; n.color_mode = (chctlb >> 1) & 1; break;
        default: n.char_size = (chctlb >> 4) & 1; n.bitmap = false; n.color_mode = (chctlb >> 5) & 1; break;
    }
    n.bmp_width = (bitmap_size & 2) ? 1024 : 512;
    n.bmp_height = (bitmap_size & 1) ? 512 : 256;
    apply_pnc(n, g.u16(kPncn0 + 2 * index));

    const uint16_t plsz = g.u16(kPlsz);
    const int size = (plsz >> (2 * index)) & 3;
    n.plane_width = (size & 1) ? 2 : 1;
    n.plane_height = (size & 2) ? 2 : 1;

    const uint32_t offset = (g.u16(kMpofn) >> (4 * index)) & 7;
    for (int p = 0; p < 4; ++p) {
        const uint16_t reg = g.u16(kMpabn0 + index * 4 + (p / 2) * 2);
        const uint32_t v = (p & 1) ? (reg >> 8) & 0x3F : reg & 0x3F;
        n.map[p] = (offset << 6) | v;
    }
    n.bmp_addr = offset * 0x20000;
    if (index < 2) {
        const uint16_t bmpna = uint16_t(g.u16(kBmpna) >> (8 * index));
        n.bmp_palette = uint32_t(bmpna & 7) << 8;
        n.bmp_special_priority = bmpna & 0x20;
        n.bmp_special_cc = bmpna & 0x10;
    }

    // Rolagem: parte inteira de 11 bits e fração nos 8 bits altos da palavra seguinte (só NBG0/NBG1 têm fração e zoom).
    const auto scroll = [&](uint32_t integer, uint32_t fraction) { return (int(g.u16(integer) & 0x7FF) << 8) | (fraction ? (g.u16(fraction) >> 8) & 0xFF : 0); };
    if (index == 0) { n.scroll_x = scroll(kScroll0, kScroll0 + 2); n.scroll_y = scroll(kScroll0 + 4, kScroll0 + 6); }
    else if (index == 1) { n.scroll_x = scroll(kScroll1, kScroll1 + 2); n.scroll_y = scroll(kScroll1 + 4, kScroll1 + 6); }
    else if (index == 2) { n.scroll_x = scroll(kScrollN2, 0); n.scroll_y = scroll(kScrollN2 + 2, 0); }
    else { n.scroll_x = scroll(kScrollN3, 0); n.scroll_y = scroll(kScrollN3 + 2, 0); }

    if (index < 2) {
        // Zoom: incremento 3.8; 0 vale 1,0. O ZMCTL limita a redução: ZMQT (até 1/4), ZMHF (até 1/2), nenhum (sem redução).
        const uint16_t zmctl = uint16_t(g.u16(kZmctl) >> (8 * index));
        n.zoom_x_max = (zmctl & 2) ? 1024 : (zmctl & 1) ? 512 : 256;
        const uint32_t zoom = kZoom0 + 0x10 * index;
        const auto step = [&](uint32_t integer) {
            const int v = ((g.u16(integer) & 7) << 8) | ((g.u16(integer + 2) >> 8) & 0xFF);
            return v == 0 ? 256 : std::min(v, n.zoom_x_max);
        };
        n.zoom_x = step(zoom);
        n.zoom_y = step(zoom + 4);

        const uint16_t scrctl = uint16_t(g.u16(kScrctl) >> (8 * index));
        n.vcell = scrctl & 1;
        n.line_x = scrctl & 2;
        n.line_y = scrctl & 4;
        n.line_zoom = scrctl & 8;
        n.line_shift = (scrctl >> 4) & 3;
        const uint32_t lsta = kLsta0 + 4 * index;
        n.line_addr = ((uint32_t(g.u16(lsta) & 7) << 16) | g.u16(lsta + 2)) * 2;
        n.vcell_addr = ((uint32_t(g.u16(kVcsta) & 7) << 16) | g.u16(kVcsta + 2)) * 2;
    }

    const uint16_t prina = g.u16(kPrina), prinb = g.u16(kPrinb);
    n.priority = index == 0 ? prina & 7 : index == 1 ? (prina >> 8) & 7 : index == 2 ? prinb & 7 : (prinb >> 8) & 7;
    n.cram_offset = uint32_t((g.u16(kCraofa) >> (4 * index)) & 7) << 8;
    n.special_priority = (g.u16(kSfprmd) >> (2 * index)) & 3;
    n.cc_enabled = g.u16(kCcctl) & (1 << index);
    n.cc_per_char = ((g.u16(kSfccmd) >> (2 * index)) & 3) == 1;
    n.cc_ratio = (g.u16(kCcrna + 2 * (index / 2)) >> (8 * (index & 1))) & 0x1F;
    n.window = uint8_t(g.u16(kWctla + 2 * (index / 2)) >> (8 * (index & 1)));
    return n;
}

// ---- Camadas de rotação (RBG0/RBG1) ----

int sext(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

// Tabela de parâmetros de rotação (um conjunto, A ou B), lida da VRAM. Frações: .10 salvo indicação.
struct RotParam {
    int xst = 0, yst = 0, zst = 0, dxst = 0, dyst = 0, dx = 0, dy = 0;
    std::array<int, 6> m{};                  // matriz A..F
    int px = 0, py = 0, pz = 0, cx = 0, cy = 0, cz = 0;   // inteiros
    int mx = 0, my = 0;
    int kx = 0x10000, ky = 0x10000;          // .16
    uint32_t kast = 0, dkast = 0, dkax = 0;  // .10
    // Controle da tabela de coeficientes (KTCTL, KTAOF)
    bool use_coeff = false, one_word = false;
    int coeff_mode = 0;                      // 0: kx e ky, 1: kx, 2: ky, 3: Xp
    uint32_t coeff_base = 0;                 // KTAOF << 26 (soma-se a KAst)
};

RotParam read_rot_param(const Vdp2State& s, uint32_t base) {
    RotParam r;
    const auto v32 = [&](uint32_t off) { return vram32(s.vram, base + off); };
    r.xst = sext(v32(0x00) >> 6, 23); r.yst = sext(v32(0x04) >> 6, 23); r.zst = sext(v32(0x08) >> 6, 23);
    r.dxst = sext(v32(0x0C) >> 6, 13); r.dyst = sext(v32(0x10) >> 6, 13);
    r.dx = sext(v32(0x14) >> 6, 13); r.dy = sext(v32(0x18) >> 6, 13);
    for (uint32_t k = 0; k < 6; ++k) r.m[k] = sext(v32(0x1C + 4 * k) >> 6, 14);
    const auto v16 = [&](uint32_t off) { return sext(vram16(s.vram, base + off), 14); };
    r.px = v16(0x34); r.py = v16(0x36); r.pz = v16(0x38);
    r.cx = v16(0x3C); r.cy = v16(0x3E); r.cz = v16(0x40);
    r.mx = sext(v32(0x44) >> 6, 24); r.my = sext(v32(0x48) >> 6, 24);
    r.kx = sext(v32(0x4C), 24); r.ky = sext(v32(0x50), 24);
    r.kast = v32(0x54) >> 6; r.dkast = uint32_t(sext(v32(0x58) >> 6, 20)); r.dkax = uint32_t(sext(v32(0x5C) >> 6, 20));
    return r;
}

// Valores de um parâmetro já combinados para uma linha de tela (matriz, ponto de partida e deslocamentos por ponto).
struct RotLine { int64_t xsp = 0, ysp = 0, xp = 0, yp = 0, dx = 0, dy = 0; uint32_t kast = 0; };

RotLine make_rot_line(const RotParam& r, int y) {
    const int64_t xs = r.xst + int64_t(r.dxst) * y - (int64_t(r.px) << 10);
    const int64_t ys = r.yst + int64_t(r.dyst) * y - (int64_t(r.py) << 10);
    const int64_t zs = r.zst - (int64_t(r.pz) << 10);
    const auto& m = r.m;
    RotLine l;
    l.xsp = (m[0] * xs + m[1] * ys + m[2] * zs) >> 10;
    l.ysp = (m[3] * xs + m[4] * ys + m[5] * zs) >> 10;
    const int64_t ox = r.px - r.cx, oy = r.py - r.cy, oz = r.pz - r.cz;
    l.xp = m[0] * ox + m[1] * oy + m[2] * oz + (int64_t(r.cx) << 10) + r.mx;
    l.yp = m[3] * ox + m[4] * oy + m[5] * oz + (int64_t(r.cy) << 10) + r.my;
    l.dx = (int64_t(m[0]) * r.dx + int64_t(m[1]) * r.dy) >> 10;
    l.dy = (int64_t(m[3]) * r.dx + int64_t(m[4]) * r.dy) >> 10;
    l.kast = r.coeff_base + r.kast + r.dkast * uint32_t(y);
    return l;
}

struct RotFrame {
    std::array<RotParam, 2> param;
    std::array<RotLine, 2> line;
    int rpmd = 0;                            // RPMD: 0 = A, 1 = B, 2 = por coeficiente de A, 3 = por janela
    bool coeff_in_cram = false;              // RAMCTL CRKTE
};

// Coeficiente do ponto: bit 31 = transparente, bits 23-0 = valor (s.7.16). Lido da VRAM ou da RAM de cores (CRKTE).
uint32_t read_coeff(const Vdp2State& s, const RotFrame& f, const RotParam& r, uint32_t kast) {
    uint32_t index = kast >> 10;
    if (!r.one_word) index <<= 1;
    const auto word = [&](uint32_t i) -> uint16_t {
        if (f.coeff_in_cram) return uint16_t((s.cram[(0x800 + ((i & 0x3FF) << 1)) % s.cram.size()] << 8) | s.cram[(0x801 + ((i & 0x3FF) << 1)) % s.cram.size()]);
        return vram16(s.vram, (i & 0x3FFFF) << 1);
    };
    if (r.one_word) {
        const uint16_t t = word(index);
        return (uint32_t(sext(uint32_t(t) << 6, 21)) & 0xFFFFFF) | (uint32_t(t & 0x8000) << 16);
    }
    return (uint32_t(word(index)) << 16) | word(index + 1);
}

// Parâmetro, coeficiente e posição de um ponto: devolve as coordenadas na camada e se o coeficiente o torna transparente.
struct RotDot { int64_t kx, ky, xp, yp; bool transparent; };

RotDot rot_dot(const Vdp2State& s, const RotFrame& f, int ab, int x) {
    const RotParam& r = f.param[ab];
    const RotLine& l = f.line[ab];
    RotDot d{r.kx, r.ky, l.xp, l.yp, false};
    if (!r.use_coeff) return d;
    const uint32_t c = read_coeff(s, f, r, l.kast + r.dkax * uint32_t(x));
    d.transparent = c & 0x80000000u;
    const int64_t v = sext(c & 0xFFFFFF, 24);
    switch (r.coeff_mode) {
        case 0: d.kx = d.ky = v; break;
        case 1: d.kx = v; break;
        case 2: d.ky = v; break;
        default: d.xp = v << 2; break;
    }
    return d;
}

// Cor do ponto (ix, iy) do plano de rotação (coordenadas inteiras sem sinal); 0 = transparente.
uint32_t rot_pixel(const Vdp2State& s, const Nbg& n, const ColorLut& lut, uint32_t ix, uint32_t iy, uint8_t& flags) {
    flags = 0;
    const uint32_t wide = uint32_t(n.plane_width - 1), tall = uint32_t(n.plane_height - 1);
    // Área coberta pela camada: fora dela vale o modo de estouro (0 repete, 1 padrão fixo, 2 e 3 transparente).
    uint32_t out_x = 0, out_y = 0;
    if (n.rot_over == 3) out_x = out_y = ~511u;
    else if (n.rot_over != 0 && n.bitmap) { out_x = ~511u; out_y = ~uint32_t(n.bmp_height - 1); }
    else if (n.rot_over != 0) { out_x = ~((1u << (11 + wide)) - 1); out_y = ~((1u << (11 + tall)) - 1); }
    const bool outside = (ix & out_x) | (iy & out_y);
    if (outside && (n.rot_over & 2)) return kNoPixel;

    if (n.bitmap) {
        flags = uint8_t(n.bmp_special_priority) | (n.bmp_special_cc << 1);
        return bitmap_pixel(s, n, lut, int(ix & 511), int(iy & uint32_t(n.bmp_height - 1)));
    }
    Cell cell;
    if (outside && n.rot_over == 1) {
        cell = decode_one_word(n, n.rot_over_pn);
    } else {
        const uint32_t cs = n.char_size;
        const uint32_t map_index = ((ix >> (9 + wide)) & 3) | (((iy >> (9 + tall)) & 3) << 2);
        const uint32_t size_bits = wide | (tall << 1);
        const uint32_t page = (n.rot_map[map_index] & ~size_bits) + (((ix >> 9) & wide) | (((iy >> 9) & tall) << 1));
        const uint32_t entry_bytes = n.one_word ? 2 : 4;
        const uint32_t page_bytes = (64u >> cs) * (64u >> cs) * entry_bytes;
        const uint32_t entry = (((ix >> 3) & 0x3F) >> cs) + ((((iy >> 3) & 0x3F) >> cs) << (6 - cs));
        const uint32_t addr = page * page_bytes + entry * entry_bytes;
        cell = n.one_word ? decode_one_word(n, vram16(s.vram, addr)) : decode_two_words(n, vram16(s.vram, addr), vram16(s.vram, addr + 2));
    }
    flags = uint8_t(cell.spr) | (cell.scc << 1);
    const int mask = (n.char_size ? 16 : 8) - 1;
    return cell_color(s, n, lut, cell, int(ix) & mask, int(iy) & mask);
}

// Preenche uma linha de camada de rotação. `use_b` (ou nulo) é a escolha por janela do parâmetro B no modo RPMD = 3.
void fill_rot_row(const Vdp2State& s, const Nbg& n, const ColorLut& lut, const RotFrame& f, int width, const uint8_t* use_b, uint32_t* color, uint8_t* flags) {
    for (int x = 0; x < width; ++x) {
        int ab = n.rot_param;
        if (n.rot_param == 0) {   // RBG0: o RPMD escolhe o parâmetro; o RBG1 fica sempre no B
            if (f.rpmd == 1) ab = 1;
            else if (f.rpmd == 3 && use_b) ab = use_b[x];
        }
        RotDot d = rot_dot(s, f, ab, x);
        if (n.rot_param == 0 && f.rpmd == 2 && f.param[0].use_coeff && d.transparent) {   // coeficiente de A com o bit 31: usa o B
            ab = 1;
            d = rot_dot(s, f, 1, x);
        }
        const RotLine& l = f.line[ab];
        const uint32_t ix = uint32_t(d.xp + ((d.kx * (l.xsp + l.dx * x)) >> 16)) >> 10;
        const uint32_t iy = uint32_t(d.yp + ((d.ky * (l.ysp + l.dy * x)) >> 16)) >> 10;
        flags[x] = 0;
        color[x] = d.transparent ? kNoPixel : rot_pixel(s, n, lut, ix, iy, flags[x]);
    }
}

// Plano de mapa de rotação do parâmetro `param`: tamanho, estouro, 16 planos e deslocamento de mapa.
void read_rot_map(const Regs& g, Nbg& n, int param) {
    const uint16_t plsz = g.u16(kPlsz);
    const int size = (plsz >> (8 + 4 * param)) & 3;
    n.plane_width = (size & 1) ? 2 : 1;
    n.plane_height = (size & 2) ? 2 : 1;
    n.rot_over = (plsz >> (10 + 4 * param)) & 3;
    n.rot_over_pn = g.u16(kOvpnr + 2 * param);
    n.rot_param = param;
    n.rotating = true;
    const uint32_t offset = (g.u16(kMpofr) >> (4 * param)) & 7;
    for (int p = 0; p < 16; ++p) {
        const uint16_t reg = g.u16(kMpabra + 0x10 * param + (p / 2) * 2);
        const uint32_t v = (p & 1) ? (reg >> 8) & 0x3F : reg & 0x3F;
        n.rot_map[p] = (offset << 6) | (v & ~uint32_t(size) & 0x3F);
    }
    n.bmp_addr = offset * 0x20000;
}

Nbg read_rbg0(const Regs& g) {
    Nbg n;
    const uint16_t bgon = g.u16(kBgon), chctlb = g.u16(kChctlb), bmpnb = g.u16(kBmpnb), ccctl = g.u16(kCcctl);
    n.enabled = bgon & 0x10;
    n.transparent_code_visible = bgon & 0x1000;
    n.char_size = (chctlb >> 8) & 1;
    n.bitmap = chctlb & 0x200;
    n.bmp_width = 512;
    n.bmp_height = (chctlb & 0x400) ? 512 : 256;
    n.color_mode = (chctlb >> 12) & 7;
    apply_pnc(n, g.u16(kPncr));
    n.bmp_palette = uint32_t(bmpnb & 7) << 8;
    n.bmp_special_priority = bmpnb & 0x20;
    n.bmp_special_cc = bmpnb & 0x10;
    read_rot_map(g, n, 0);
    n.priority = g.u16(kPrir) & 7;
    n.cram_offset = uint32_t(g.u16(kCraofb) & 7) << 8;
    n.special_priority = (g.u16(kSfprmd) >> 8) & 3;
    n.cc_enabled = ccctl & 0x10;
    n.cc_per_char = ((g.u16(kSfccmd) >> 8) & 3) == 1;
    n.cc_ratio = g.u16(kCcrr) & 0x1F;
    n.window = uint8_t(g.u16(kWctla + 4));
    return n;
}

// RBG1 usa os registradores de NBG0 (formato, prioridade, janela, cálculo de cor) e o parâmetro B.
Nbg read_rbg1(const Regs& g) {
    Nbg n = read_nbg(g, 0);
    n.enabled = g.u16(kBgon) & 0x20;
    n.bitmap = false;
    n.vcell = n.line_x = n.line_y = n.line_zoom = false;
    read_rot_map(g, n, 1);
    return n;
}

RotFrame read_rot_frame(const Regs& g, const Vdp2State& s) {
    RotFrame f;
    f.rpmd = g.u16(kRpmd) & 3;
    f.coeff_in_cram = g.u16(kRamctl) & 0x8000;
    const uint32_t table = (((uint32_t(g.u16(kRptau) & 7) << 16) | g.u16(kRptau + 2)) & 0x7FFBE) * 2;
    const uint16_t ktctl = g.u16(kKtctl), ktaof = g.u16(kKtaof);
    for (int i = 0; i < 2; ++i) {
        RotParam& r = f.param[i];
        r = read_rot_param(s, table + 0x80 * uint32_t(i));
        const unsigned ctl = (ktctl >> (8 * i)) & 0x1F;
        r.use_coeff = ctl & 1;
        r.one_word = ctl & 2;
        r.coeff_mode = (ctl >> 2) & 3;
        r.coeff_base = uint32_t((ktaof >> (8 * i)) & 7) << 26;
    }
    return f;
}

// Janelas W0/W1 (retangulares, com X por linha opcional) e a janela de sprite.
struct Windows {
    std::array<int, 2> left{}, top{}, right{}, bottom{};
    std::array<bool, 2> line_enable{};       // janela de linha (LWTAn bit 15): X inicial/final por linha, lidos da VRAM
    std::array<uint32_t, 2> line_addr{};
    bool high_res = false;

    // X em meios pixels fora do modo alta resolução.
    int scale_x(uint32_t v) const { return high_res ? int(v & 0x3FF) : int(v & 0x3FF) >> 1; }

    // Posições horizontais da linha `y`: as janelas de linha leem o par (início, fim) da tabela, uma entrada de 4 bytes por linha.
    Windows for_row(const Vdp2State& s, int y) const {
        Windows w = *this;
        for (int i = 0; i < 2; ++i) {
            if (!line_enable[i]) continue;
            const uint32_t entry = line_addr[i] + 4 * uint32_t(y);
            w.left[i] = scale_x(vram16(s.vram, entry));
            w.right[i] = scale_x(vram16(s.vram, entry + 2));
        }
        return w;
    }

    // Combinação das áreas de W0/W1 e da janela de sprite para uma camada. `ctl` é o byte de WCTLx: bit 1/3 liga W0/W1, bit 0/2
    // escolhe a área (0 dentro, 1 fora), bit 5 liga a janela de sprite (bit 4 = área), bit 7 = E (senão OU).
    // Devolve verdadeiro se o ponto fica escondido.
    static bool combine(uint8_t ctl, bool inside0, bool inside1, bool sprite_window) {
        bool any = false, all = true, some = false;
        const auto add = [&](bool hidden) { any = true; all = all && hidden; some = some || hidden; };
        if (ctl & 2) add((ctl & 1) ? !inside0 : inside0);
        if (ctl & 8) add((ctl & 4) ? !inside1 : inside1);
        if (ctl & 0x20) add((ctl & 0x10) ? !sprite_window : sprite_window);
        return any && ((ctl & 0x80) ? all : some);
    }
};

// Regra de janela de uma camada, resolvida uma vez por quadro: tabela indexada por (dentro de W0, dentro de W1, bit da janela de sprite).
struct WindowRule {
    bool active = false;
    std::array<bool, 8> hidden{};
    explicit WindowRule(uint8_t ctl = 0) : active(ctl & 0x2A) {
        for (int i = 0; i < 8; ++i) hidden[i] = Windows::combine(ctl, i & 4, i & 2, i & 1);
    }
    // `y0`/`y1` = y dentro da faixa vertical de W0/W1, calculado uma vez por linha.
    bool hides(const Windows& w, int x, bool y0, bool y1, bool sprite_window) const {
        const bool in0 = y0 && x >= w.left[0] && x <= w.right[0], in1 = y1 && x >= w.left[1] && x <= w.right[1];
        return hidden[(in0 << 2) | (in1 << 1) | sprite_window];
    }
};

Windows read_windows(const Regs& g, bool high_res) {
    Windows w;
    w.high_res = high_res;
    for (int i = 0; i < 2; ++i) {
        const uint32_t base = kWindowPos + 8 * i;
        w.left[i] = w.scale_x(g.u16(base)); w.top[i] = g.u16(base + 2) & 0x1FF; w.right[i] = w.scale_x(g.u16(base + 4)); w.bottom[i] = g.u16(base + 6) & 0x1FF;
        const uint32_t lwta = kLwta0 + 4 * i;
        w.line_enable[i] = g.u16(lwta) & 0x8000;
        w.line_addr[i] = ((uint32_t(g.u16(lwta) & 7) << 16) | (g.u16(lwta + 2) & 0xFFFE)) * 2;
    }
    return w;
}

// Divisão dos 16 bits do ponto de sprite por tipo (SPCTL bits 3-0): prioridade, razão de cálculo de cor e código de cor.
struct SpriteType { int pr_shift, pr_bits, cc_shift, cc_bits, dc_bits; };
constexpr std::array<SpriteType, 16> kSpriteTypes{{
    {14, 2, 11, 3, 11}, {13, 3, 11, 2, 11}, {14, 1, 11, 3, 11}, {13, 2, 11, 2, 11},
    {13, 2, 10, 3, 10}, {12, 3, 11, 1, 11}, {12, 3, 10, 2, 10}, {12, 3, 9, 3, 9},
    {7, 1, 0, 0, 7}, {7, 1, 6, 1, 6}, {6, 2, 0, 0, 6}, {0, 0, 6, 2, 6},
    {7, 1, 0, 0, 8}, {7, 1, 6, 1, 8}, {6, 2, 0, 0, 8}, {0, 0, 6, 2, 8},
}};
constexpr int field(uint16_t v, int shift, int bits) { return bits ? (v >> shift) & ((1 << bits) - 1) : 0; }

uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }
int channel(uint32_t c, int i) { return (c >> (8 * i)) & 0xFF; }

uint32_t blend_add(uint32_t top, uint32_t second) {
    return rgba(clamp8(channel(top, 0) + channel(second, 0)), clamp8(channel(top, 1) + channel(second, 1)), clamp8(channel(top, 2) + channel(second, 2)));
}
// Razão r (0–31): a camada de cima pesa (32 - r)/32 e a de baixo r/32.
uint32_t blend_ratio(uint32_t top, uint32_t second, int r) {
    const auto mix = [&](int i) { return uint8_t((channel(top, i) * (32 - r) + channel(second, i) * r) / 32); };
    return rgba(mix(0), mix(1), mix(2));
}

// Deslocamento de cor (registradores A/B): 9 bits com sinal por canal. Cada conjunto vira três tabelas de 256 entradas (com saturação).
struct ColorOffset {
    std::array<int, 3> a{}, b{};
    struct Table { bool identity = true; std::array<std::array<uint8_t, 256>, 3> map{}; };
    Table table_a, table_b;

    void build() {
        const auto make = [](const std::array<int, 3>& o, Table& t) {
            t.identity = o[0] == 0 && o[1] == 0 && o[2] == 0;
            for (int ch = 0; ch < 3; ++ch)
                for (int v = 0; v < 256; ++v) t.map[ch][v] = clamp8(v + o[ch]);
        };
        make(a, table_a);
        make(b, table_b);
    }
    uint32_t apply(uint32_t c, bool use_b) const {
        const Table& t = use_b ? table_b : table_a;
        if (t.identity) return c;
        return rgba(t.map[0][channel(c, 0)], t.map[1][channel(c, 1)], t.map[2][channel(c, 2)]);
    }
};

ColorOffset read_color_offset(const Regs& g) {
    const auto s9 = [](uint16_t v) { return (v & 0x100) ? int(v & 0x1FF) - 0x200 : int(v & 0xFF); };
    ColorOffset o;
    for (int i = 0; i < 3; ++i) { o.a[i] = s9(g.u16(kCoa + 2 * i)); o.b[i] = s9(g.u16(kCob + 2 * i)); }
    o.build();
    return o;
}

}  // namespace

Frame render_vdp2(const Vdp2State& s, unsigned layer_mask, const SpriteFrame* sprites) {
    const Regs g{s.regs};
    const uint16_t tvmd = g.u16(kTvmd);
    const int hres = tvmd & 7, vres = (tvmd >> 4) & 3;
    const int width = (hres & 1) ? 352 : 320, height = vres == 0 ? 224 : vres == 1 ? 240 : 256;
    Frame frame(width, height);
    if (!(tvmd & 0x8000)) return frame;

    const int cram_mode = (g.u16(kRamctl) >> 12) & 3;
    ColorLut lut;
    for (uint32_t i = 0; i < lut.size(); ++i) lut[i] = cram_color(s.cram, cram_mode, i);
    const uint32_t back_addr = ((uint32_t(g.u16(kBktau) & 7) << 16) | g.u16(kBktau + 2)) * 2;
    const bool back_per_line = g.u16(kBktau) & 0x8000;    // BKCLMD: uma cor por linha
    const uint32_t back_fixed = rgb555(vram16(s.vram, back_addr));

    // Camadas: NBG0..NBG3 e RBG0 (índice 4). Com RBG1 ligado ele ocupa o lugar de NBG0 e as outras NBGs não são exibidas.
    std::array<Nbg, 5> layers;
    for (int i = 0; i < 4; ++i) layers[i] = read_nbg(g, i);
    layers[4] = read_rbg0(g);
    if (g.u16(kBgon) & 0x20) {
        layers[0] = read_rbg1(g);
        for (int i = 1; i < 4; ++i) layers[i].enabled = false;
    }
    const bool rotating = layers[4].enabled || layers[0].rotating;
    RotFrame rot;
    if (rotating) rot = read_rot_frame(g, s);
    // A tabela de rolagem vertical por célula é compartilhada: com NBG0 e NBG1 ligadas, as entradas se alternam (NBG0 primeiro).
    const uint32_t vcell_stride = (layers[0].vcell && layers[1].vcell) ? 8 : 4;
    layers[0].vcell_stride = layers[1].vcell_stride = vcell_stride;
    layers[1].vcell_offset = layers[0].vcell ? 4 : 0;

    const uint16_t spctl = g.u16(kSpctl), ccctl = g.u16(kCcctl), clofen = g.u16(kClofen), clofsl = g.u16(kClofsl);
    const SpriteType& sprite_type = kSpriteTypes[spctl & 0xF];
    const bool sprite_rgb_allowed = spctl & 0x20;
    const int sprite_cc_condition = (spctl >> 12) & 3, sprite_cc_number = (spctl >> 8) & 7;
    const uint32_t sprite_cram_offset = uint32_t((g.u16(kCraofb) >> 4) & 7) << 8;
    const uint8_t sprite_window = uint8_t(g.u16(kWctla + 4) >> 8);
    const bool add_mode = ccctl & 0x100;
    std::array<int, 8> sprite_priority{}, sprite_ratio{};
    for (int i = 0; i < 8; ++i) {
        sprite_priority[i] = (g.u16(kPrisa + 2 * (i / 2)) >> (8 * (i & 1))) & 7;
        sprite_ratio[i] = (g.u16(kCcrsa + 2 * (i / 2)) >> (8 * (i & 1))) & 0x1F;
    }
    const Windows windows = read_windows(g, hres & 2);
    const bool any_line_window = windows.line_enable[0] || windows.line_enable[1];
    const ColorOffset offset = read_color_offset(g);

    // Camadas candidatas por pixel; a ordem de desempate em prioridade igual é sprite > NBG0 > NBG1 > NBG2 > NBG3.
    // As chaves são todas distintas entre camadas, então a ordem em que as camadas são oferecidas não altera o resultado.
    // (RBG0 vem logo abaixo do sprite e acima das NBGs.)
    enum : int { kSprite = 4, kBack = 5, kRbg0 = 6 };
    const auto key = [](int priority, int layer) { return priority * 8 + (layer == kSprite ? 7 : layer == kRbg0 ? 6 : 5 - layer); };
    const auto offset_bit = [](int layer) { return layer == kSprite ? 6 : layer == kBack ? 5 : layer == kRbg0 ? 4 : layer; };

    // Camadas que realmente contribuem; as desligadas ou de prioridade 0 não são nem renderizadas.
    std::array<bool, 5> active{};
    for (int i = 0; i < 5; ++i) active[i] = layers[i].enabled && layers[i].priority != 0 && (layer_mask & (i == 4 ? 0x20u : 1u << i));
    const bool sprites_active = sprites && (layer_mask & 0x10);
    const WindowRule sprite_rule(sprite_window);
    std::array<WindowRule, 5> rules;
    for (int i = 0; i < 5; ++i) rules[i] = WindowRule(layers[i].window);
    const WindowRule rot_select_rule(uint8_t(g.u16(kWctla + 6)));   // RPMD = 3: a janela escolhe A (fora) ou B (dentro da regra)
    // Bit de janela do sprite (SPCTL SPWINEN): bit 15 do ponto nos tipos 2 a 7, os únicos em que ele não faz parte do dado.
    const bool sprite_window_enabled = sprites && (spctl & 0x10) && (spctl & 0xF) >= 2 && (spctl & 0xF) <= 7;

    // Estado por ponto da linha: a camada vencedora (chave, cor, camada, cálculo de cor, razão) e a cor da segunda colocada.
    struct Px { int top_key, second_key; uint32_t top_color, second_color; uint8_t top_layer, top_ratio; bool top_cc; };
    std::array<Px, 352> st;
    // A segunda colocada só importa para o cálculo de cor; sem ele, basta acompanhar a vencedora.
    bool needs_second = ccctl & 0x40;
    for (const Nbg& n : layers) needs_second = needs_second || n.cc_enabled;
    const auto offer = [&st, needs_second](int x, int k, uint32_t color, int layer, bool cc, int ratio) {
        Px& p = st[x];
        if (k > p.top_key) {
            if (needs_second) { p.second_key = p.top_key; p.second_color = p.top_color; }
            p.top_key = k; p.top_color = color; p.top_layer = uint8_t(layer); p.top_cc = cc; p.top_ratio = uint8_t(ratio);
        } else if (needs_second && k > p.second_key) {
            p.second_key = k; p.second_color = color;
        }
    };

    uint32_t memo_in = 0, memo_out = 0;
    int memo_bit = -1;
    std::array<uint32_t, 352> row_color;
    std::array<uint8_t, 352> row_flags;
    std::array<uint8_t, 352> sprite_window_bit{}, rot_use_b{};
    for (int y = 0; y < height; ++y) {
        const uint32_t back = back_per_line ? rgb555(vram16(s.vram, back_addr + 2 * uint32_t(y))) : back_fixed;
        const bool y0 = y >= windows.top[0] && y <= windows.bottom[0], y1 = y >= windows.top[1] && y <= windows.bottom[1];
        const Windows row_windows = any_line_window ? windows.for_row(s, y) : windows;
        if (sprite_window_enabled) {
            sprite_window_bit.fill(0);
            if (y < SpriteFrame::kHeight)
                for (int x = 0; x < std::min(width, int(SpriteFrame::kWidth)); ++x) sprite_window_bit[x] = sprites->data[size_t(y) * SpriteFrame::kWidth + x] >> 15;
        }
        if (rotating) {
            for (int p = 0; p < 2; ++p) rot.line[p] = make_rot_line(rot.param[p], y);
            if (rot.rpmd == 3)
                for (int x = 0; x < width; ++x) rot_use_b[x] = rot_select_rule.active && rot_select_rule.hides(row_windows, x, y0, y1, sprite_window_bit[x]);
        }
        for (int x = 0; x < width; ++x) {
            st[x] = {-1, -1, back, back, kBack, 0, false};
        }
        for (int i = 0; i < 5; ++i) {
            if (!active[i]) continue;
            const Nbg& n = layers[i];
            if (n.rotating) fill_rot_row(s, n, lut, rot, width, rot_use_b.data(), row_color.data(), row_flags.data());
            else fill_row(s, n, lut, y, width, row_color.data(), row_flags.data());
            const int layer_id = i == 4 ? int(kRbg0) : i;
            const WindowRule& rule = rules[i];
            const bool per_dot_priority = n.special_priority == 1;
            const int fixed_key = key(n.priority, layer_id);
            for (int x = 0; x < width; ++x) {
                if (row_color[x] == kNoPixel) continue;
                if (rule.active && rule.hides(row_windows, x, y0, y1, sprite_window_bit[x])) continue;
                int k = fixed_key;
                if (per_dot_priority) {
                    const int priority = (n.priority & 6) | (row_flags[x] & 1);
                    if (priority == 0) continue;
                    k = key(priority, layer_id);
                }
                offer(x, k, row_color[x], layer_id, n.cc_enabled && (!n.cc_per_char || (row_flags[x] & 2)), n.cc_ratio);
            }
        }
        if (sprites_active && y < SpriteFrame::kHeight) {
            const uint16_t* sprite_row = &sprites->data[size_t(y) * SpriteFrame::kWidth];
            const int limit = std::min(width, int(SpriteFrame::kWidth));
            for (int x = 0; x < limit; ++x) {
                const uint16_t v = sprite_row[x];
                if (v == 0 || (sprite_window_enabled && !(v & 0x7FFF)) || (sprite_rule.active && sprite_rule.hides(row_windows, x, y0, y1, sprite_window_bit[x]))) continue;
                const bool rgb = sprite_rgb_allowed && (v & 0x8000);
                const int dc = field(v, 0, sprite_type.dc_bits);
                if (!rgb && dc == 0) continue;
                const int pr_index = rgb ? 0 : field(v, sprite_type.pr_shift, sprite_type.pr_bits);
                const int cc_index = rgb ? 0 : field(v, sprite_type.cc_shift, sprite_type.cc_bits);
                const int priority = sprite_priority[pr_index];
                if (priority == 0) continue;
                bool cc = ccctl & 0x40;
                switch (sprite_cc_condition) {
                    case 0: cc = cc && priority <= sprite_cc_number; break;
                    case 1: cc = cc && priority == sprite_cc_number; break;
                    case 2: cc = cc && priority >= sprite_cc_number; break;
                    default: cc = cc && (v & 0x8000); break;
                }
                offer(x, key(priority, kSprite), rgb ? rgb555(v) : lut_color(lut, sprite_cram_offset + uint32_t(dc)), kSprite, cc, sprite_ratio[cc_index]);
            }
        }
        for (int x = 0; x < width; ++x) {
            const Px& p = st[x];
            uint32_t color = p.top_color;
            if (p.top_cc) color = add_mode ? blend_add(color, p.second_color) : blend_ratio(color, p.second_color, p.top_ratio);
            const int bit = offset_bit(p.top_layer);
            if (clofen & (1 << bit)) {
                // Memória da última conversão: trechos de cor repetida (comuns) evitam refazer o deslocamento por ponto.
                if (color != memo_in || bit != memo_bit) { memo_in = color; memo_bit = bit; memo_out = offset.apply(color, clofsl & (1 << bit)); }
                color = memo_out;
            }
            frame.at(x, y) = color;
        }
    }
    return frame;
}

}  // namespace video
