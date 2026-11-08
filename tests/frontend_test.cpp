#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include "frontend/config.hpp"
#include "frontend/font.hpp"
#include "frontend/input.hpp"
#include "frontend/menu.hpp"
#include "frontend/states.hpp"

using namespace frontend;

namespace {

std::filesystem::path temp_dir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("dxpc-frontend-test-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

struct Fixture {
    Config config;
    int saved = -1, loaded = -1;
    bool quit = false, fail = false;
    Menu menu;
    Fixture() : menu(config, make_host()) { menu.open(); }
    MenuHost make_host() {
        MenuHost h;
        h.slot_text = [](int slot) { return slot == 2 ? std::string("01/02/2026 10:00") : std::string("vazio"); };
        h.save_state = [this](int s) { if (fail) throw std::runtime_error("falhou"); saved = s; };
        h.load_state = [this](int s) { if (fail) throw std::runtime_error("falhou"); loaded = s; };
        h.quit = [this] { quit = true; };
        h.key_name = [](int code) { return "K" + std::to_string(code); };
        return h;
    }
    void go(std::initializer_list<Nav> navs) { for (Nav n : navs) menu.handle(n); }
    std::string label(int row) const { return menu.rows()[row].label; }
    std::string value(int row) const { return menu.rows()[row].value; }
};

int count_pixels(const GlyphRows& rows) {
    int n = 0;
    for (auto r : rows) for (int b = 0; b < 8; ++b) n += (r >> b) & 1;
    return n;
}

}  // namespace

TEST_CASE("fonte: ASCII imprimível tem glifos distintos e cabem em 5 colunas", "[frontend][font]") {
    std::set<std::array<uint8_t, kGlyphHeight>> seen;
    for (char32_t c = 33; c <= 126; ++c) {
        const auto rows = glyph_rows(c);
        REQUIRE(count_pixels(rows) > 0);
        for (auto r : rows) REQUIRE((r & ~0x1F) == 0);
        REQUIRE(seen.insert(rows).second);
    }
    REQUIRE(count_pixels(glyph_rows(U' ')) == 0);
}

TEST_CASE("fonte: acentos do português existem e diferem da letra base", "[frontend][font]") {
    for (const char32_t c : std::u32string(U"áàâãéêíóôõúçÁÀÂÃÉÊÍÓÔÕÚÇ")) {
        REQUIRE(has_glyph(c));
        REQUIRE(count_pixels(glyph_rows(c)) > 0);
    }
    REQUIRE(glyph_rows(U'á') != glyph_rows(U'a'));
    REQUIRE(glyph_rows(U'ç') != glyph_rows(U'c'));
    REQUIRE(glyph_rows(U'Ã') != glyph_rows(U'A'));
    REQUIRE(glyph_rows(U'á') != glyph_rows(U'à'));
    REQUIRE(glyph_rows(U'ã') != glyph_rows(U'õ'));
    REQUIRE(glyph_rows(U'中') == glyph_rows(U'?'));   // desconhecido vira '?'
}

TEST_CASE("fonte: UTF-8 e largura do texto", "[frontend][font]") {
    REQUIRE(decode_utf8("ação") == U"ação");
    REQUIRE(text_width("ação") == 4 * kGlyphWidth);
    REQUIRE(decode_utf8("a\xFF" "b").size() == 3);   // byte inválido não derruba
    REQUIRE(decode_utf8("\xC3").size() == 1);        // sequência cortada
}

TEST_CASE("fonte: draw_text pinta pixels com a cor e respeita o recorte", "[frontend][font]") {
    video::Frame f(40, 16);
    const uint32_t white = video::rgba(255, 255, 255);
    const int end = draw_text(f, 2, 1, "Hi", white);
    REQUIRE(end == 2 + 2 * kGlyphWidth);
    int lit = 0;
    for (auto p : f.pixels) lit += p == white;
    REQUIRE(lit > 10);
    video::Frame g(40, 16);
    draw_text(g, 0, 0, "Hello", white, 12);   // cabem só 2 glifos
    for (int y = 0; y < 16; ++y) for (int x = 12; x < 40; ++x) REQUIRE(g.at(x, y) == 0);
}

TEST_CASE("fonte: mistura de alfa", "[frontend][font]") {
    REQUIRE(blend_pixel(video::rgba(100, 100, 100), video::rgba(0, 0, 0, 0)) == video::rgba(100, 100, 100));
    REQUIRE(blend_pixel(video::rgba(100, 100, 100), video::rgba(200, 0, 0, 255)) == video::rgba(200, 0, 0));
    const uint32_t half = blend_pixel(video::rgba(200, 200, 200), video::rgba(0, 0, 0, 128));
    REQUIRE((half & 0xFF) >= 99);
    REQUIRE((half & 0xFF) <= 101);
}

TEST_CASE("config: ida e volta preserva tudo", "[frontend][config]") {
    Config c;
    c.fullscreen = true; c.integer_scale = false; c.aspect = Aspect::k4x3; c.filter = Filter::kSmooth; c.scanlines = true;
    c.vsync = true; c.window_scale = 5; c.volume = 45; c.mute = true; c.latency = Latency::kHigh; c.deadzone = 20; c.slot = 6;
    c.bindings.keys[kA] = 100; c.bindings.pad[kB] = kPadRightTrigger; c.bindings.keys[kL] = kUnbound;
    REQUIRE(parse_config(serialize_config(c)) == c);
}

TEST_CASE("config: valores inválidos caem no padrão sem travar", "[frontend][config]") {
    const Config d;
    const Config c = parse_config("volume = abc\nwindow_scale = 99\nfilter = lindo\naspect = 4:3\nlixo sem igual\nkey.a = -5\nkey.zzz = 3\n"
                                  "pad.b = 5000\ndeadzone = 1\nfullscreen = talvez\nslot = 9\n\x01\x02 = =\n=\n");
    REQUIRE(c.volume == d.volume);
    REQUIRE(c.window_scale == d.window_scale);
    REQUIRE(c.filter == d.filter);
    REQUIRE(c.aspect == Aspect::k4x3);   // o válido do meio do lixo vale
    REQUIRE(c.bindings == d.bindings);
    REQUIRE(c.deadzone == d.deadzone);
    REQUIRE(c.fullscreen == d.fullscreen);
    REQUIRE(c.slot == d.slot);
    REQUIRE(parse_config("") == d);
    REQUIRE(parse_config("volume = 47").volume == 45);   // passos de 5
}

TEST_CASE("config: arquivo gravado com troca atômica", "[frontend][config]") {
    const auto dir = temp_dir("config");
    const auto file = dir / "sub" / "dracula-x.ini";
    REQUIRE(load_config(file) == Config{});   // ausente: padrão
    Config c;
    c.volume = 20;
    REQUIRE(save_config(file, c));
    REQUIRE(!std::filesystem::exists(file.string() + ".tmp"));
    REQUIRE(load_config(file).volume == 20);
    std::filesystem::remove_all(dir);
}

TEST_CASE("config: retângulo da imagem", "[frontend][config]") {
    Config c;
    Rect r = game_rect(c, 1280, 720);   // inteira: 3x (960x672)
    REQUIRE(r.w == 960);
    REQUIRE(r.h == 672);
    REQUIRE(r.x == 160);
    REQUIRE(r.y == 24);
    c.integer_scale = false;
    r = game_rect(c, 1280, 720);   // 720/224 = 3,2143
    REQUIRE(r.h == 720);
    REQUIRE(r.w == 1029);
    c.aspect = Aspect::k4x3;
    r = game_rect(c, 1280, 720);
    REQUIRE(r.h == 720);
    REQUIRE(r.w == 960);   // 4:3 exato
    c.aspect = Aspect::kStretch;
    r = game_rect(c, 1280, 720);
    REQUIRE((r.x == 0 && r.y == 0 && r.w == 1280 && r.h == 720));
    c.aspect = Aspect::kOriginal;
    c.integer_scale = true;
    r = game_rect(c, 200, 100);   // menor que 1x: ainda cabe
    REQUIRE(r.w <= 200);
    REQUIRE(r.h <= 100);
}

TEST_CASE("input: padrão equivale ao mapeamento antigo e mistura teclado com controle", "[frontend][input]") {
    const Bindings b = default_bindings();
    const auto none = [](int) { return false; };
    REQUIRE(map_inputs(b, none, none) == 0);
    REQUIRE(map_inputs(b, [&](int k) { return k == b.keys[kStart] || k == b.keys[kA]; }, none) == (0x0800 | 0x0400));
    REQUIRE(map_inputs(b, none, [&](int c) { return c == kPadLeftTrigger || c == kPadRightTrigger; }) == (0x0040 | 0x0010));
    REQUIRE(map_inputs(b, [&](int k) { return k == b.keys[kUp]; }, [&](int c) { return c == b.pad[kUp]; }) == 0x1000);
    Bindings custom = b;
    custom.keys[kB] = 99;
    custom.pad[kB] = kUnbound;
    REQUIRE(map_inputs(custom, [](int k) { return k == 99; }, none) == pad_bit(kB));
    std::set<uint16_t> bits;
    for (int i = 0; i < kButtonCount; ++i) bits.insert(pad_bit(i));
    REQUIRE(bits.size() == size_t(kButtonCount));
}

TEST_CASE("input: analógico com zona morta e repetição", "[frontend][input]") {
    REQUIRE(stick_bits(0, 0, 35) == 0);
    REQUIRE(stick_bits(32767, 0, 35) == pad_bit(kRight));
    REQUIRE(stick_bits(-32768, -32768, 35) == (pad_bit(kLeft) | pad_bit(kUp)));
    REQUIRE(stick_bits(0, 15000, 50) == 0);
    REQUIRE(stick_bits(0, 15000, 20) == pad_bit(kDown));
    Repeater r(400, 100);
    REQUIRE(r.update(true, 0));
    REQUIRE(!r.update(true, 200));
    REQUIRE(r.update(true, 400));
    REQUIRE(!r.update(true, 450));
    REQUIRE(r.update(true, 500));
    REQUIRE(!r.update(false, 510));
    REQUIRE(r.update(true, 520));
}

TEST_CASE("menu: navegação, pular cabeçalhos e voltar/fechar", "[frontend][menu]") {
    Fixture f;
    REQUIRE(f.menu.is_open());
    REQUIRE(f.label(0) == "Continuar");
    REQUIRE(f.menu.selected() == 0);
    f.go({Nav::kUp});   // dá a volta
    REQUIRE(f.label(f.menu.selected()) == "Sair do jogo");
    f.go({Nav::kDown});
    REQUIRE(f.menu.selected() == 0);
    f.go({Nav::kDown, Nav::kDown, Nav::kConfirm});   // Vídeo
    REQUIRE(f.menu.title() == "Menu > Vídeo");
    f.go({Nav::kBack});
    REQUIRE(f.menu.title() == "Menu");
    f.go({Nav::kBack});   // na raiz, fecha
    REQUIRE(!f.menu.is_open());
    f.menu.open();
    f.go({Nav::kConfirm});   // Continuar
    REQUIRE(!f.menu.is_open());
}

TEST_CASE("menu: ajuste de valores de vídeo e áudio", "[frontend][menu]") {
    Fixture f;
    REQUIRE(f.menu.open_path({"Vídeo"}));
    REQUIRE(f.value(0) == "Janela");
    f.go({Nav::kRight});
    REQUIRE(f.config.fullscreen);
    REQUIRE(f.menu.take_changed());
    REQUIRE(!f.menu.take_changed());
    f.go({Nav::kDown, Nav::kConfirm});   // enter alterna
    REQUIRE(!f.config.integer_scale);
    f.go({Nav::kDown, Nav::kRight});
    REQUIRE(f.config.aspect == Aspect::k4x3);
    f.go({Nav::kLeft, Nav::kLeft});
    REQUIRE(f.config.aspect == Aspect::kStretch);   // dá a volta
    f.go({Nav::kDown, Nav::kRight});
    REQUIRE(f.config.filter == Filter::kSmooth);
    f.go({Nav::kDown, Nav::kRight, Nav::kDown, Nav::kRight});
    REQUIRE(f.config.scanlines);
    REQUIRE(f.config.vsync);
    f.go({Nav::kDown});
    for (int i = 0; i < 20; ++i) f.go({Nav::kRight});
    REQUIRE(f.config.window_scale == 8);
    REQUIRE(f.value(f.menu.selected()) == "8x");

    REQUIRE(f.menu.open_path({"Áudio"}));
    f.go({Nav::kLeft});
    REQUIRE(f.config.volume == 95);
    for (int i = 0; i < 30; ++i) f.go({Nav::kLeft});
    REQUIRE(f.config.volume == 0);
    f.go({Nav::kDown, Nav::kRight, Nav::kDown, Nav::kLeft});
    REQUIRE(f.config.mute);
    REQUIRE(f.config.latency == Latency::kLow);
    REQUIRE(latency_frames(Latency::kLow) == 2);
    REQUIRE(latency_frames(Latency::kMedium) == 3);
    REQUIRE(latency_frames(Latency::kHigh) == 6);
}

TEST_CASE("menu: save states salvam, carregam e mostram erros", "[frontend][menu]") {
    Fixture f;
    REQUIRE(f.menu.open_path({"Save states"}));
    REQUIRE(f.menu.row_count() == kSlotCount + 1);
    REQUIRE(f.value(2) == "01/02/2026 10:00");
    REQUIRE(f.value(0) == "* vazio");   // slot atual marcado
    f.menu.select(2);
    f.go({Nav::kConfirm});   // entra no slot 3
    REQUIRE(f.config.slot == 2);
    REQUIRE(f.menu.title() == "Menu > Save states > Slot 3");
    REQUIRE(f.value(0) == "01/02/2026 10:00");
    f.go({Nav::kConfirm});   // a seleção já cai em "Salvar" (pula o cabeçalho)
    REQUIRE(f.saved == 2);
    REQUIRE(f.menu.is_open());
    REQUIRE(!f.menu.message_is_error());
    f.fail = true;
    f.go({Nav::kDown, Nav::kConfirm});   // carregar falha
    REQUIRE(f.loaded == -1);
    REQUIRE(f.menu.is_open());
    REQUIRE(f.menu.message_is_error());
    REQUIRE(f.menu.message().find("falhou") != std::string::npos);
    f.fail = false;
    f.go({Nav::kConfirm});
    REQUIRE(f.loaded == 2);
    REQUIRE(!f.menu.is_open());   // carregar fecha o menu
}

TEST_CASE("menu: sair pede confirmação", "[frontend][menu]") {
    Fixture f;
    REQUIRE(f.menu.open_path({"Sair do jogo"}));
    REQUIRE(f.label(f.menu.selected()) == "Não, voltar");
    f.go({Nav::kConfirm});
    REQUIRE(!f.quit);
    REQUIRE(f.menu.title() == "Menu");
    REQUIRE(f.menu.open_path({"Sair do jogo"}));
    f.go({Nav::kDown, Nav::kConfirm});
    REQUIRE(f.quit);
}

TEST_CASE("menu: remapear teclado e controle, Esc cancela, troca em duplicata", "[frontend][menu]") {
    Fixture f;
    REQUIRE(f.menu.open_path({"Controles", "Teclado"}));
    REQUIRE(f.label(kStart) == "Start");
    f.menu.select(kStart);
    f.go({Nav::kConfirm});
    REQUIRE(f.menu.capturing());
    REQUIRE(f.value(kStart) == "pressione a tecla...");
    f.go({Nav::kDown});   // ignorado em captura
    REQUIRE(f.menu.selected() == kStart);
    f.menu.capture_key(kScancodeEscape);
    REQUIRE(!f.menu.capturing());
    REQUIRE(f.config.bindings.keys[kStart] == default_bindings().keys[kStart]);

    f.go({Nav::kConfirm});
    f.menu.capture_key(50);
    REQUIRE(!f.menu.capturing());
    REQUIRE(f.config.bindings.keys[kStart] == 50);
    REQUIRE(f.value(kStart) == "K50");
    // usar a tecla de outro botão troca os dois
    const int a_key = f.config.bindings.keys[kA];
    f.go({Nav::kConfirm});
    f.menu.capture_key(a_key);
    REQUIRE(f.config.bindings.keys[kStart] == a_key);
    REQUIRE(f.config.bindings.keys[kA] == 50);

    f.go({Nav::kBack});
    REQUIRE(f.menu.open_path({"Controles", "Controle"}));
    f.menu.select(kB);
    f.go({Nav::kConfirm});
    f.menu.capture_key(60);   // teclado não vale na captura de controle
    REQUIRE(f.menu.capturing());
    f.menu.capture_pad(kPadRightTrigger);
    REQUIRE(f.config.bindings.pad[kB] == kPadRightTrigger);
    REQUIRE(f.config.bindings.pad[kZ] == default_bindings().pad[kB]);   // Z perdeu o gatilho e ficou com o antigo do B
    f.menu.select(kButtonCount);   // restaurar padrão
    f.go({Nav::kConfirm});
    REQUIRE(f.config.bindings.pad == default_bindings().pad);
}

TEST_CASE("menu: zona morta e restaurar tudo", "[frontend][menu]") {
    Fixture f;
    f.config.bindings.keys[kA] = 77;
    REQUIRE(f.menu.open_path({"Controles"}));
    f.menu.select(2);
    f.go({Nav::kLeft});
    REQUIRE(f.config.deadzone == 30);
    f.menu.select(3);
    f.go({Nav::kConfirm});
    REQUIRE(f.config.bindings == default_bindings());
    REQUIRE(f.config.deadzone == Config{}.deadzone);
}

TEST_CASE("menu: mouse (hover, clique, roda, linha sob o ponto) e rolagem", "[frontend][menu]") {
    Fixture f;
    const int y1 = kMenuRowsTop + kMenuRowHeight + 3;
    REQUIRE(f.menu.row_at(100, y1) == 1);
    REQUIRE(f.menu.row_at(2, y1) == -1);
    REQUIRE(f.menu.row_at(100, 5) == -1);
    REQUIRE(f.menu.row_at(100, kMenuRowsTop + 6 * kMenuRowHeight + 1) == -1);   // abaixo do último item
    f.menu.hover(3);
    REQUIRE(f.menu.selected() == 3);
    f.menu.click(2);   // Vídeo
    REQUIRE(f.menu.title() == "Menu > Vídeo");
    f.menu.hover(6);
    f.menu.wheel(+1, 6);   // roda para cima aumenta
    REQUIRE(f.config.window_scale == 4);
    f.menu.wheel(-1, 6);
    f.menu.wheel(-1, 6);
    REQUIRE(f.config.window_scale == 2);
    f.menu.wheel(-1, -1);   // fora de linha: rola a seleção
    REQUIRE(f.menu.selected() != 6);

    REQUIRE(f.menu.open_path({"Controles", "Teclado"}));   // 15 linhas: rola
    f.go({Nav::kUp});
    REQUIRE(f.menu.selected() == kButtonCount + 1);
    REQUIRE(f.menu.scroll() == 15 - kMenuVisibleRows);
    REQUIRE(f.menu.row_at(100, kMenuRowsTop) == f.menu.scroll());
    f.go({Nav::kPageUp});
    REQUIRE(f.menu.selected() == 1);
    f.menu.click(0);
    REQUIRE(f.menu.capturing());
    f.menu.click(0);   // clique cancela a captura
    REQUIRE(!f.menu.capturing());
}

TEST_CASE("menu: renderização do overlay", "[frontend][menu]") {
    Fixture f;
    video::Frame overlay(kOverlayW, kOverlayH);
    draw_menu(f.menu, overlay);
    int opaque = 0;
    for (auto p : overlay.pixels) opaque += (p >> 24) == 255;
    REQUIRE(opaque > 500);               // texto
    REQUIRE((overlay.at(0, 0) >> 24) == 175);   // fundo escurecido
    video::Frame game(kOverlayW, kOverlayH);
    std::fill(game.pixels.begin(), game.pixels.end(), video::rgba(200, 200, 200));
    composite(game, overlay);
    REQUIRE((game.at(0, 0) & 0xFF) < 100);
    draw_toast(overlay, "Estado salvo no slot 1", false);
    f.menu.set_message("Erro: " + std::string(200, 'x'), true);
    draw_menu(f.menu, overlay);   // mensagem longa não estoura
}

TEST_CASE("states: caminho, gravação atômica e descrição", "[frontend][states]") {
    const auto dir = temp_dir("states");
    const auto file = state_path(dir, 0);
    REQUIRE(file.filename() == "slot1.dxs");
    REQUIRE(state_path(dir, 7).filename() == "slot8.dxs");
    REQUIRE(slot_description(file) == "vazio");
    const std::vector<uint8_t> data{1, 2, 3, 4};
    write_file_atomic(file, data);
    REQUIRE(!std::filesystem::exists(file.string() + ".tmp"));
    REQUIRE(read_file_bytes(file) == data);
    const std::vector<uint8_t> data2{9};
    write_file_atomic(file, data2);
    REQUIRE(read_file_bytes(file) == data2);
    const std::string text = slot_description(file);
    REQUIRE(text.size() == 16);   // dd/mm/aaaa hh:mm
    REQUIRE(text[2] == '/');
    REQUIRE(text[13] == ':');
    REQUIRE_THROWS_AS(read_file_bytes(dir / "nao-existe"), std::runtime_error);
    std::filesystem::remove_all(dir);
}
