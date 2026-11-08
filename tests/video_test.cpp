#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

#include "iso_builder.hpp"
#include "video/frame.hpp"
#include "video/vdp1.hpp"
#include "video/vdp2.hpp"

using namespace video;

namespace {

struct Vdp2Rig {
    std::vector<uint8_t> vram = std::vector<uint8_t>(0x80000, 0), cram = std::vector<uint8_t>(0x1000, 0), regs = std::vector<uint8_t>(0x120, 0);
    void reg(uint32_t off, uint16_t v) { regs[off] = uint8_t(v >> 8); regs[off + 1] = uint8_t(v); }
    void word(uint32_t addr, uint16_t v) { vram[addr] = uint8_t(v >> 8); vram[addr + 1] = uint8_t(v); }
    void color(uint32_t index, uint16_t v) { cram[index * 2] = uint8_t(v >> 8); cram[index * 2 + 1] = uint8_t(v); }
    Frame render(unsigned mask = 0x3F, const SpriteFrame* sprites = nullptr) { return render_vdp2({vram, cram, regs}, mask, sprites); }
};

}  // namespace

TEST_CASE("VDP2: display desligado devolve um quadro preto de 320x224") {
    Vdp2Rig rig;
    const Frame f = rig.render();
    CHECK(f.width == 320);
    CHECK(f.height == 224);
    CHECK((f.at(10, 10) & 0xFFFFFF) == 0u);
}

TEST_CASE("VDP2: NBG0 de 16 cores, pattern de 1 palavra, mostra o pixel da célula com a cor da paleta") {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);              // DISP, 320x224
    rig.reg(0x20, 0x0001);              // NBG0 ligado
    rig.reg(0x30, 0x8000);              // pattern de 1 palavra, sem suplemento
    rig.reg(0xF8, 0x0001);              // prioridade 1
    rig.reg(0x40, 0x0000);              // mapas na página 0
    rig.word(0, 0x0001);                // célula (0,0): caractere 1, paleta 0
    rig.vram[0x20] = 0x20;              // caractere 1 (0x20 bytes): primeiro pixel = código 2
    rig.color(2, 0x001F);               // cor 2 = vermelho puro
    const Frame f = rig.render();
    CHECK(f.at(0, 0) == rgba(255, 0, 0));
    CHECK(f.at(1, 0) == f.at(100, 100));  // código 0 é transparente: aparece o fundo
}

TEST_CASE("VDP2: o número de caractere suplementar completa os 15 bits no modo de 1 palavra (CNSM=0)") {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0x20, 0x0001);
    rig.reg(0x30, 0x8003);              // 1 palavra, SCN = 3: caractere + (3 << 10)
    rig.reg(0xF8, 0x0001);
    rig.word(0, 0x0001);
    rig.vram[(0xC01u) * 0x20] = 0x10;   // caractere 0xC01: primeiro pixel = código 1
    rig.color(1, 0x03E0);               // verde
    CHECK(rig.render().at(0, 0) == rgba(0, 255, 0));
}

TEST_CASE("VDP1: polígono preenche o retângulo com o código de cor, respeitando a coordenada local") {
    std::vector<uint8_t> vram(0x80000, 0);
    const auto put = [&](uint32_t at, uint16_t v) { vram[at] = uint8_t(v >> 8); vram[at + 1] = uint8_t(v); };
    put(0x00, 0x000A); put(0x0C, 10); put(0x0E, 20);                           // coordenada local (10, 20)
    put(0x20, 0x0004); put(0x26, 0x8123);                                      // polígono com a cor 0x8123
    put(0x2C, 0); put(0x2E, 0); put(0x30, 3); put(0x32, 0); put(0x34, 3); put(0x36, 3); put(0x38, 0); put(0x3A, 3);
    put(0x40, 0x8000);                                                         // END
    const SpriteFrame out = render_vdp1(vram);
    CHECK(out.data[size_t(20) * SpriteFrame::kWidth + 10] == 0x8123);
    CHECK(out.data[size_t(23) * SpriteFrame::kWidth + 13] == 0x8123);
    CHECK(out.data[size_t(24) * SpriteFrame::kWidth + 14] == 0);
}

TEST_CASE("PNG: o arquivo gerado começa com a assinatura e tem o IHDR com a largura e a altura") {
    Frame f(4, 2);
    f.at(1, 1) = rgba(1, 2, 3);
    const auto path = testsupport::make_temp_dir("png") / "a.png";
    write_png(path, f);
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(bytes.size() > 33);
    CHECK(std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(std::memcmp(bytes.data() + 12, "IHDR", 4) == 0);
    CHECK(bytes[19] == 4);   // largura (big-endian, byte baixo)
    CHECK(bytes[23] == 2);   // altura
}

namespace {
// NBG0 de 16 cores cobrindo a tela inteira: todas as células usam o caractere 1, todo preenchido com o código 2.
Vdp2Rig solid_nbg0(uint16_t color2) {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0x20, 0x0001);
    rig.reg(0x30, 0x8000);
    rig.reg(0xF8, 0x0004);                         // NBG0 com prioridade 4
    rig.reg(0xAE, 0x8000);                         // tabela do fundo em 0x10000 (preto)
    for (uint32_t e = 0; e < 64 * 64; ++e) rig.word(e * 2, 0x0001);
    for (uint32_t b = 0; b < 32; ++b) rig.vram[0x20 + b] = 0x22;
    rig.color(2, color2);
    return rig;
}
SpriteFrame sprite_at(int x, int y, uint16_t value) {
    SpriteFrame s;
    s.data[size_t(y) * SpriteFrame::kWidth + x] = value;
    return s;
}
}  // namespace

TEST_CASE("VDP2: sprite tipo 5 lê a prioridade nos bits 14-12") {
    Vdp2Rig rig = solid_nbg0(0x001F);              // NBG0 vermelho, prioridade 4
    rig.reg(0xE0, 0x0005);                         // SPCTL: tipo 5
    rig.reg(0xF4, 0x0005);                         // PRISC: registrador de prioridade 4 = 5
    rig.color(5, 0x03E0);
    const SpriteFrame s = sprite_at(3, 3, 0x4005); // bits 14-12 = 4, cor 5
    CHECK(rig.render(0x1F, &s).at(3, 3) == rgba(0, 255, 0));
}

TEST_CASE("VDP2: a janela 0 com área externa esconde a NBG fora do retângulo") {
    Vdp2Rig rig = solid_nbg0(0x001F);
    rig.reg(0xC0, 0); rig.reg(0xC2, 12); rig.reg(0xC4, 319 * 2); rig.reg(0xC6, 223);   // W0: x 0-319 (unidades de meio pixel), y 12-223
    rig.reg(0xD0, 0x0003);                                                            // WCTLA: NBG0 com W0 ligada, área = fora
    const Frame f = rig.render();
    CHECK(f.at(5, 5) == rgba(0, 0, 0));
    CHECK(f.at(5, 20) == rgba(255, 0, 0));
}

TEST_CASE("VDP2: o deslocamento de cor A soma ao canal da camada habilitada") {
    Vdp2Rig rig = solid_nbg0(0x0000 | 0x0400);     // azul fraco (B = 1 → 8)
    rig.reg(0x110, 0x0001);                        // CLOFEN: NBG0
    rig.reg(0x114, 0x0040);                        // COAR = +64
    rig.reg(0x118, 0x01F8);                        // COAB = -8 (9 bits com sinal)
    CHECK(rig.render().at(0, 0) == rgba(64, 0, 0));
}

TEST_CASE("VDP2: o deslocamento de CRAM da NBG0 (CRAOFA) desloca o índice da paleta em 256") {
    Vdp2Rig rig = solid_nbg0(0x001F);
    rig.reg(0xE4, 0x0001);
    rig.color(256 + 2, 0x7C00);                    // azul
    CHECK(rig.render().at(0, 0) == rgba(0, 0, 255));
}

TEST_CASE("VDP2: cálculo de cor do sprite em modo soma mistura com a camada de baixo") {
    Vdp2Rig rig = solid_nbg0(0x001F);              // vermelho
    rig.reg(0xE0, 0x0705);                         // SPCTL: tipo 5, SPCCCS = 0 (prioridade <= 7), SPCCN = 7
    rig.reg(0xF4, 0x0005);
    rig.reg(0xEC, 0x0140);                         // CCCTL: SPCCEN + CCMD (soma)
    rig.color(5, 0x03E0);                          // verde
    const SpriteFrame s = sprite_at(3, 3, 0x4005);
    CHECK(rig.render(0x1F, &s).at(3, 3) == rgba(255, 255, 0));
}

TEST_CASE("VDP2: cálculo de cor por razão da NBG0 sobre o fundo preto") {
    Vdp2Rig rig = solid_nbg0(0x001F);
    rig.reg(0xEC, 0x0001);                         // N0CCEN, modo razão
    rig.reg(0x108, 16);                            // CCRNA: NBG0 = 16/32 da camada de baixo
    CHECK(rig.render().at(0, 0) == rgba(127, 0, 0));        // (255·16 + 0·16) / 32, truncado
}

namespace {
std::vector<uint8_t> vdp1_list(std::initializer_list<std::pair<uint32_t, uint16_t>> words) {
    std::vector<uint8_t> vram(0x80000, 0);
    for (auto [at, v] : words) { vram[at] = uint8_t(v >> 8); vram[at + 1] = uint8_t(v); }
    return vram;
}
uint16_t fb(const SpriteFrame& f, int x, int y) { return f.data[size_t(y) * SpriteFrame::kWidth + x]; }
}  // namespace

TEST_CASE("VDP1: a malha (mesh) desenha só os pixels com x+y par") {
    const auto vram = vdp1_list({{0x00, 0x0004}, {0x04, 0x0100}, {0x06, 0x8001},
                                 {0x0C, 0}, {0x0E, 0}, {0x10, 3}, {0x12, 0}, {0x14, 3}, {0x16, 3}, {0x18, 0}, {0x1A, 3}, {0x20, 0x8000}});
    const SpriteFrame out = render_vdp1(vram);
    CHECK(fb(out, 0, 0) == 0x8001);
    CHECK(fb(out, 1, 0) == 0);
    CHECK(fb(out, 1, 1) == 0x8001);
}

TEST_CASE("VDP1: o recorte de usuário limita o desenho por dentro (Cmod=0) ou por fora (Cmod=1)") {
    const auto make = [](uint16_t pmod) {
        return vdp1_list({{0x00, 0x0008}, {0x0C, 10}, {0x0E, 10}, {0x14, 20}, {0x16, 20},          // recorte de usuário (10,10)-(20,20)
                          {0x20, 0x0004}, {0x24, pmod}, {0x26, 0x8001},
                          {0x2C, 0}, {0x2E, 0}, {0x30, 30}, {0x32, 0}, {0x34, 30}, {0x36, 30}, {0x38, 0}, {0x3A, 30}, {0x40, 0x8000}});
    };
    const SpriteFrame inside = render_vdp1(make(0x0400));
    CHECK(fb(inside, 15, 15) == 0x8001);
    CHECK(fb(inside, 5, 5) == 0);
    const SpriteFrame outside = render_vdp1(make(0x0600));
    CHECK(fb(outside, 15, 15) == 0);
    CHECK(fb(outside, 5, 5) == 0x8001);
    const SpriteFrame off = render_vdp1(make(0x0000));
    CHECK(fb(off, 5, 5) == 0x8001);
    CHECK(fb(off, 15, 15) == 0x8001);
}

// ---------- Rolagem, zoom, bitmap e fundo por linha do VDP2 ----------
namespace {
void word32(Vdp2Rig& rig, uint32_t addr, uint32_t v) { rig.word(addr, uint16_t(v >> 16)); rig.word(addr + 2, uint16_t(v)); }

// NBG0 8x8 de 16 cores; mapa na página 1 (0x2000). A célula (0,0) usa o caractere 1, cuja única parte desenhada é dada por `pixels`
// (linha, coluna) com o código 2 (vermelho). Todas as outras células usam o caractere 0 (transparente).
Vdp2Rig dot_nbg0(std::initializer_list<std::pair<int, int>> pixels) {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0x20, 0x0001);
    rig.reg(0x30, 0x8000);
    rig.reg(0xF8, 0x0004);
    rig.reg(0x40, 0x0001);
    rig.word(0x2000, 0x0001);
    for (auto [row, col] : pixels) rig.vram[0x20 + row * 4 + col / 2] |= (col & 1) ? 0x02 : 0x20;
    rig.color(2, 0x001F);
    return rig;
}
const uint32_t kRed = rgba(255, 0, 0), kBlack = rgba(0, 0, 0);
}  // namespace

TEST_CASE("VDP2: rolagem por linha (LSCX) desloca cada linha pelo valor da tabela") {
    Vdp2Rig rig = dot_nbg0({{0, 7}, {1, 7}, {2, 7}});
    rig.reg(0x9A, 0x0002);                         // SCRCTL: N0LSCX, intervalo de 1 linha
    rig.reg(0xA0, 0x0000); rig.reg(0xA2, 0x8000);  // LSTA0 = 0x10000 bytes (endereço em palavras 0x8000)
    word32(rig, 0x10000, 0); word32(rig, 0x10004, 3u << 16); word32(rig, 0x10008, 5u << 16);
    const Frame f = rig.render();
    CHECK(f.at(7, 0) == kRed);
    CHECK(f.at(4, 1) == kRed);
    CHECK(f.at(7, 1) == kBlack);
    CHECK(f.at(2, 2) == kRed);
}

TEST_CASE("VDP2: o intervalo da rolagem por linha (LSS) repete a entrada em várias linhas") {
    Vdp2Rig rig = dot_nbg0({{0, 7}, {1, 7}, {2, 7}, {3, 7}});
    rig.reg(0x9A, 0x0012);                         // N0LSCX, intervalo de 2 linhas
    rig.reg(0xA0, 0x0000); rig.reg(0xA2, 0x8000);
    word32(rig, 0x10000, 0); word32(rig, 0x10004, 2u << 16);
    const Frame f = rig.render();
    CHECK(f.at(7, 0) == kRed);
    CHECK(f.at(7, 1) == kRed);
    CHECK(f.at(5, 2) == kRed);
    CHECK(f.at(5, 3) == kRed);
}

TEST_CASE("VDP2: rolagem vertical por célula (VCSC) usa um valor por coluna de 8 pontos") {
    Vdp2Rig rig = dot_nbg0({});
    for (int c = 0; c < 8; ++c) rig.vram[0x20 + 3 * 4 + c / 2] = 0x22;     // só a linha 3 do caractere 1 é desenhada
    rig.word(0x2002, 0x0001);                      // célula (1,0) também usa o caractere 1
    rig.reg(0x9A, 0x0001);                         // N0VCSC
    rig.reg(0x9C, 0x0000); rig.reg(0x9E, 0x8000);  // VCSTA = 0x10000
    word32(rig, 0x10000, 0); word32(rig, 0x10004, 2u << 16);
    const Frame f = rig.render();
    CHECK(f.at(0, 3) == kRed);
    CHECK(f.at(0, 1) == kBlack);
    CHECK(f.at(8, 1) == kRed);
    CHECK(f.at(8, 3) == kBlack);
}

TEST_CASE("VDP2: zoom horizontal de NBG0 com incremento 0,5 amplia por 2") {
    Vdp2Rig rig = dot_nbg0({{0, 7}});
    rig.reg(0x7A, 0x8000);                         // ZMXDN0 = 0,5
    const Frame f = rig.render();
    CHECK(f.at(14, 0) == kRed);
    CHECK(f.at(15, 0) == kRed);
    CHECK(f.at(7, 0) == kBlack);
    CHECK(f.at(13, 0) == kBlack);
}

TEST_CASE("VDP2: zoom vertical de NBG0 com incremento 0,5 amplia por 2") {
    Vdp2Rig rig = dot_nbg0({{3, 0}});
    rig.reg(0x7E, 0x8000);                         // ZMYDN0 = 0,5
    const Frame f = rig.render();
    CHECK(f.at(0, 6) == kRed);
    CHECK(f.at(0, 7) == kRed);
    CHECK(f.at(0, 3) == kBlack);
    CHECK(f.at(0, 5) == kBlack);
}

TEST_CASE("VDP2: redução 2x exige ZMCTL (sem o bit o incremento fica limitado a 1)") {
    Vdp2Rig rig = dot_nbg0({{0, 7}});
    rig.reg(0x70, 1);                              // SCXIN0 = 1
    rig.reg(0x78, 2);                              // ZMXIN0 = 2 (reduz pela metade)
    CHECK(rig.render().at(6, 0) == kRed);          // sem ZMCTL: incremento limitado a 1, nada muda
    rig.reg(0x98, 0x0001);                         // N0ZMHF
    const Frame f = rig.render();
    CHECK(f.at(3, 0) == kRed);                     // 1 + 2*3 = 7
    CHECK(f.at(7, 0) == kBlack);
}

TEST_CASE("VDP2: a parte fracionária da rolagem (SCXDN) conta junto com o zoom") {
    Vdp2Rig rig = dot_nbg0({{0, 7}});
    rig.reg(0x70, 1); rig.reg(0x72, 0x8000);       // rolagem 1,5
    rig.reg(0x7A, 0x8000);                         // incremento 0,5
    const Frame f = rig.render();
    CHECK(f.at(10, 0) == kBlack);
    CHECK(f.at(11, 0) == kRed);
    CHECK(f.at(12, 0) == kRed);
    CHECK(f.at(13, 0) == kBlack);
}

namespace {
Vdp2Rig bitmap_nbg0(uint16_t chctla) {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0x20, 0x0001);
    rig.reg(0x28, chctla);
    rig.reg(0xF8, 0x0004);
    rig.color(3, 0x001F);
    return rig;
}
}  // namespace

TEST_CASE("VDP2: NBG0 em modo bitmap 512x256 de 256 cores") {
    Vdp2Rig rig = bitmap_nbg0(0x0012);
    rig.vram[5 * 512 + 10] = 3;
    const Frame f = rig.render();
    CHECK(f.at(10, 5) == kRed);
    CHECK(f.at(11, 5) == kBlack);
    CHECK(f.at(10, 6) == kBlack);
}

TEST_CASE("VDP2: bitmap 1024x256 usa a largura de 1024 e rolagem") {
    Vdp2Rig rig = bitmap_nbg0(0x001A);             // tamanho 2 (1024x256), 256 cores
    rig.vram[3 * 1024 + 700] = 3;
    rig.reg(0x70, 600);
    const Frame f = rig.render();
    CHECK(f.at(100, 3) == kRed);
    CHECK(f.at(101, 3) == kBlack);
}

TEST_CASE("VDP2: bitmap de 32k cores exige o bit 15 do ponto e 512x512 usa 512 linhas") {
    Vdp2Rig rig = bitmap_nbg0(0x0036);             // tamanho 1 (512x512), 32k cores
    rig.word((300 * 512 + 2) * 2, 0x801F);
    rig.word((300 * 512 + 3) * 2, 0x001F);         // bit 15 zerado: transparente
    rig.reg(0x74, 296);
    const Frame f = rig.render();
    CHECK(f.at(2, 4) == kRed);
    CHECK(f.at(3, 4) == kBlack);
}

TEST_CASE("VDP2: bitmap de 16 cores usa a paleta de BMPNA") {
    Vdp2Rig rig = bitmap_nbg0(0x0002);             // 16 cores, 512x256
    rig.vram[0] = 0x30;                            // primeiro ponto = código 3
    rig.reg(0x2C, 0x0001);                         // paleta bits 6-4 = 1 -> índice 256 + 3
    rig.color(259, 0x03E0);
    CHECK(rig.render().at(0, 0) == rgba(0, 255, 0));
}

TEST_CASE("VDP2: cor de fundo por linha (BKCLMD) lê uma cor por linha da tabela") {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0xAC, 0x8000); rig.reg(0xAE, 0x8000);  // tabela em 0x10000, uma cor por linha
    rig.word(0x10000, 0x001F); rig.word(0x10002, 0x03E0);
    const Frame f = rig.render();
    CHECK(f.at(0, 0) == kRed);
    CHECK(f.at(5, 1) == rgba(0, 255, 0));
    CHECK(f.at(0, 2) == kBlack);
}

TEST_CASE("VDP2: em 256 cores com pattern de 2 palavras, a paleta vem dos bits 6-4 do número de paleta") {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);              // DISP, 320x224
    rig.reg(0x20, 0x0001);              // NBG0 ligado
    rig.reg(0x28, 0x0010);              // N0CHCN = 1: 256 cores
    rig.reg(0x30, 0x0000);              // pattern de 2 palavras
    rig.reg(0xF8, 0x0001);              // prioridade 1
    rig.word(0, 0x0020);                // palavra 0: número de paleta 0x20 (bits 6-4 = 2)
    rig.word(2, 0x0002);                // palavra 1: caractere 2 (0x40 bytes)
    rig.vram[0x40] = 0x05;              // primeiro pixel = código 5
    rig.color(2 * 256 + 5, 0x001F);     // paleta 2, código 5 = vermelho
    CHECK(rig.render().at(0, 0) == rgba(255, 0, 0));
}

// ---- RBG0/RBG1, janela de linha e janela de sprite ----

namespace {
constexpr uint32_t kRotTable = 0x50000;   // tabela de parâmetros de rotação (RPTA = 0x28000 palavras)
const uint32_t kGreen = rgba(0, 255, 0);

// Valor s.3.10 (matriz, deltas) gravado como a tabela espera: bits 18-6.
uint32_t fx10(double v) { return uint32_t(int32_t(v * 1024.0)) << 6; }

// Grava um conjunto de parâmetros de rotação com a matriz dada (A..F) e os incrementos ΔXst/ΔYst/ΔX/ΔY; o resto zerado, kx = ky = 1,0.
void rot_param(Vdp2Rig& rig, uint32_t base, std::array<double, 6> m = {1, 0, 0, 0, 1, 0}, double dxst = 0, double dyst = 1, double dx = 1, double dy = 0) {
    word32(rig, base + 0x0C, fx10(dxst)); word32(rig, base + 0x10, fx10(dyst));
    word32(rig, base + 0x14, fx10(dx)); word32(rig, base + 0x18, fx10(dy));
    for (uint32_t k = 0; k < 6; ++k) word32(rig, base + 0x1C + 4 * k, fx10(m[k]));
    word32(rig, base + 0x4C, 0x10000); word32(rig, base + 0x50, 0x10000);
}

// RBG0 em bitmap de 256 cores 512x256 (no início da VRAM + `bitmap_units` * 0x20000), parâmetro A com rotação identidade.
Vdp2Rig rbg0_bitmap(uint32_t bitmap_units = 0) {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0x20, 0x0010);                  // RBG0 ligado
    rig.reg(0x2A, 0x1200);                  // RBG0: bitmap, 256 cores
    rig.reg(0x3E, uint16_t(bitmap_units));  // MPOFR
    rig.reg(0xFC, 0x0004);                  // prioridade 4
    rig.reg(0xAE, 0xC000);                  // fundo em 0x18000 (preto), longe do bitmap
    rig.reg(0xBC, 0x0002); rig.reg(0xBE, 0x8000);   // RPTA: tabela em 0x50000
    rig.color(3, 0x001F);                   // vermelho
    rig.color(4, 0x03E0);                   // verde
    rot_param(rig, kRotTable);
    return rig;
}
void put_dot(Vdp2Rig& rig, int x, int y, uint8_t code, uint32_t base = 0) { rig.vram[base + uint32_t(y) * 512 + uint32_t(x)] = code; }

// RBG0 em células 8x8 de 16 cores, pattern de 1 palavra: a célula (0,0) da página 0 usa o caractere 1, que só tem o ponto (linha 0, coluna 7).
Vdp2Rig rbg0_cells() {
    Vdp2Rig rig;
    rig.reg(0x00, 0x8000);
    rig.reg(0x20, 0x0010);
    rig.reg(0x2A, 0x0000);
    rig.reg(0x38, 0x8000);                  // PNCR: 1 palavra
    rig.reg(0xFC, 0x0004);
    rig.reg(0xAE, 0xC000);
    rig.reg(0xBC, 0x0002); rig.reg(0xBE, 0x8000);
    rig.word(0, 0x0001);
    rig.vram[0x20 + 3] = 0x02;
    rig.color(2, 0x001F);
    rot_param(rig, kRotTable);
    return rig;
}
}  // namespace

TEST_CASE("VDP2: RBG0 com rotação identidade equivale à NBG0 em bitmap") {
    Vdp2Rig rbg = rbg0_bitmap();
    put_dot(rbg, 10, 5, 3); put_dot(rbg, 1, 0, 3); put_dot(rbg, 319, 223, 4); put_dot(rbg, 100, 100, 3);
    Vdp2Rig nbg = bitmap_nbg0(0x0012);
    nbg.color(4, 0x03E0);
    nbg.vram = rbg.vram;
    nbg.reg(0xAE, 0xC000);
    const Frame a = rbg.render(), b = nbg.render();
    CHECK(a.at(10, 5) == kRed);
    CHECK(a.at(11, 5) == kBlack);
    CHECK(a.at(319, 223) == kGreen);
    CHECK(a.pixels == b.pixels);
}

TEST_CASE("VDP2: RBG0 com escala 2x (matriz 0,5) amplia o bitmap") {
    Vdp2Rig rig = rbg0_bitmap();
    rot_param(rig, kRotTable, {0.5, 0, 0, 0, 0.5, 0});
    put_dot(rig, 10, 5, 3);
    const Frame f = rig.render();
    for (int y = 10; y <= 11; ++y)
        for (int x = 20; x <= 21; ++x) CHECK(f.at(x, y) == kRed);
    CHECK(f.at(19, 10) == kBlack);
    CHECK(f.at(22, 11) == kBlack);
    CHECK(f.at(20, 12) == kBlack);
}

TEST_CASE("VDP2: RBG0 girado 90 graus leva o ponto de origem (y, My - x) para a tela") {
    Vdp2Rig rig = rbg0_bitmap();
    rot_param(rig, kRotTable, {0, 1, 0, -1, 0, 0});      // X = y, Y = -x
    word32(rig, kRotTable + 0x48, 100u << 16);           // My = 100
    put_dot(rig, 7, 90, 3);                              // aparece em x = 100 - 90 = 10, y = 7
    const Frame f = rig.render();
    CHECK(f.at(10, 7) == kRed);
    CHECK(f.at(9, 7) == kBlack);
    CHECK(f.at(10, 8) == kBlack);
    CHECK(f.at(7, 10) == kBlack);
}

TEST_CASE("VDP2: tabela de coeficientes (kx por linha) e o bit de transparência") {
    Vdp2Rig rig = rbg0_bitmap();
    rig.reg(0xB4, 0x0005);                               // KTCTL A: usa coeficientes, 2 palavras, só kx
    rig.reg(0xB6, 0x0001);                               // KTAOF A = 1 -> tabela em 0x40000
    word32(rig, kRotTable + 0x58, fx10(1));              // DKAst = 1 entrada por linha
    word32(rig, 0x40000, 0x00010000);                    // linha 0: kx = 1
    word32(rig, 0x40004, 0x00020000);                    // linha 1: kx = 2
    word32(rig, 0x40008, 0x80010000);                    // linha 2: transparente
    put_dot(rig, 3, 0, 3); put_dot(rig, 2, 1, 3); put_dot(rig, 5, 2, 3);
    const Frame f = rig.render();
    CHECK(f.at(3, 0) == kRed);
    CHECK(f.at(1, 1) == kRed);
    CHECK(f.at(2, 1) == kBlack);
    CHECK(f.at(5, 2) == kBlack);
}

TEST_CASE("VDP2: RBG0 em células, e o estouro de área repete (0) ou fica transparente (2)") {
    Vdp2Rig rig = rbg0_cells();
    CHECK(rig.render().at(7, 0) == kRed);
    CHECK(rig.render().at(6, 0) == kBlack);
    word32(rig, kRotTable + 0x44, uint32_t(-4) << 16);   // Mx = -4: x < 4 cai antes do início do plano
    rig.word(63 * 2, 0x0001);                            // célula 63 da linha 0 (onde ix = ...FFFF cai ao repetir)
    CHECK(rig.render().at(3, 0) == kRed);                // repete: coluna 7 dessa célula
    rig.reg(0x3A, 0x0800);                               // PLSZ: RAOVR = 2
    CHECK(rig.render().at(3, 0) == kBlack);
    CHECK(rig.render().at(11, 0) == kRed);               // ix = 7, dentro da área
}

TEST_CASE("VDP2: o modo de estouro 1 usa o pattern name de RAOVR fora da área") {
    Vdp2Rig rig = rbg0_cells();
    word32(rig, kRotTable + 0x44, uint32_t(-4) << 16);
    rig.reg(0x3A, 0x0400);                               // RAOVR = 1
    rig.reg(0xB8, 0x0001);                               // padrão fixo = caractere 1
    CHECK(rig.render().at(3, 0) == kRed);                // fora da área: caractere 1, coluna 7
    CHECK(rig.render().at(2, 0) == kBlack);
}

TEST_CASE("VDP2: RBG0 tem prioridade sobre NBG0 de mesma prioridade") {
    Vdp2Rig rig = rbg0_bitmap(1);                        // RBG0 com o bitmap em 0x20000, prioridade 4
    rig.reg(0x20, 0x0011);                               // + NBG0 vermelha cobrindo a tela, prioridade 4
    rig.reg(0x30, 0x8000);
    rig.reg(0xF8, 0x0004);
    rig.color(2, 0x001F);
    for (uint32_t e = 0; e < 64 * 64; ++e) rig.word(e * 2, 0x0001);
    for (uint32_t b = 0; b < 32; ++b) rig.vram[0x20 + b] = 0x22;
    put_dot(rig, 12, 3, 4, 0x20000);
    const Frame f = rig.render();
    CHECK(f.at(12, 3) == kGreen);
    CHECK(f.at(13, 3) == kRed);
}

TEST_CASE("VDP2: RBG1 usa o parâmetro B e os registradores de NBG0") {
    Vdp2Rig rig = rbg0_cells();
    rig.reg(0x20, 0x0020);                               // RBG1 ligado
    rig.reg(0x30, 0x8000);                               // PNCN0: 1 palavra
    rig.reg(0xF8, 0x0004);                               // prioridade de NBG0/RBG1
    rig.reg(0xFC, 0x0000);
    for (uint32_t i = 0; i < 0x80; ++i) rig.vram[kRotTable + i] = 0;       // A zerado
    rot_param(rig, kRotTable + 0x80);                    // B identidade
    const Frame f = rig.render();
    CHECK(f.at(7, 0) == kRed);
    CHECK(f.at(6, 0) == kBlack);
}

TEST_CASE("VDP2: janela de linha lê início e fim de X de cada linha da VRAM") {
    Vdp2Rig rig = solid_nbg0(0x001F);
    rig.reg(0xC2, 0); rig.reg(0xC6, 223);                // W0 vale nas linhas 0-223
    rig.reg(0xD8, 0x8000); rig.reg(0xDA, 0xC000);        // LWTA0: tabela em 0x18000
    rig.word(0x18000, 20); rig.word(0x18002, 61);        // linha 0: x 10-30 (meios pixels)
    rig.word(0x18004, 0); rig.word(0x18006, 11);         // linha 1: x 0-5
    rig.reg(0xD0, 0x0002);                               // NBG0: W0 ligada, esconde dentro
    const Frame f = rig.render();
    CHECK(f.at(9, 0) == kRed);
    CHECK(f.at(10, 0) == kBlack);
    CHECK(f.at(30, 0) == kBlack);
    CHECK(f.at(31, 0) == kRed);
    CHECK(f.at(0, 1) == kBlack);
    CHECK(f.at(5, 1) == kBlack);
    CHECK(f.at(6, 1) == kRed);
    CHECK(f.at(10, 1) == kRed);
}

TEST_CASE("VDP2: janela de sprite esconde (ou isola) a camada pelo bit 15 do ponto do sprite") {
    Vdp2Rig rig = solid_nbg0(0x001F);
    rig.reg(0xE0, 0x0015);                               // SPCTL: tipo 5, SPWINEN
    const SpriteFrame sprites = sprite_at(10, 0, 0x8000);
    rig.reg(0xD0, 0x0020);                               // NBG0: janela de sprite, esconde onde o bit vale 1
    Frame f = rig.render(0x3F, &sprites);
    CHECK(f.at(10, 0) == kBlack);
    CHECK(f.at(11, 0) == kRed);
    CHECK(f.at(10, 1) == kRed);
    rig.reg(0xD0, 0x0030);                               // área invertida: esconde onde o bit vale 0
    f = rig.render(0x3F, &sprites);
    CHECK(f.at(10, 0) == kRed);
    CHECK(f.at(11, 0) == kBlack);
    rig.reg(0xE0, 0x0005);                               // sem SPWINEN a janela de sprite não existe
    f = rig.render(0x3F, &sprites);
    CHECK(f.at(10, 0) == kBlack);
}

TEST_CASE("VDP2: janela de sprite combinada a W0 por OU") {
    Vdp2Rig rig = solid_nbg0(0x001F);
    rig.reg(0xE0, 0x0015);
    const SpriteFrame sprites = sprite_at(10, 0, 0x8000);
    rig.reg(0xC0, 0); rig.reg(0xC2, 0); rig.reg(0xC4, 2 * 20 + 1); rig.reg(0xC6, 0);   // W0: x 0-20, y 0
    rig.reg(0xD0, 0x0022);                               // W0 (dentro) OU janela de sprite
    const Frame f = rig.render(0x3F, &sprites);
    CHECK(f.at(5, 0) == kBlack);                         // dentro de W0
    CHECK(f.at(10, 0) == kBlack);
    CHECK(f.at(21, 0) == kRed);
    CHECK(f.at(5, 1) == kRed);
}
