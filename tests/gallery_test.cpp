// Galeria da trilha sonora: catálogo a partir de um disco sintético, streaming DVI com laço, espectro, cenários e a página do menu.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <stdexcept>

#include "audio/dvi.hpp"
#include "frontend/gallery.hpp"
#include "frontend/menu.hpp"
#include "frontend/scenes.hpp"
#include "iso_builder.hpp"

using namespace frontend;

namespace {

class MemoryReader : public saturnfs::SectorReader {
public:
    explicit MemoryReader(std::vector<testsupport::Sector> sectors) : sectors_(std::move(sectors)) {}
    std::array<uint8_t, saturnfs::kUserSectorSize> read_user_sector(uint32_t lba) const override {
        if (lba >= sectors_.size()) throw std::out_of_range("setor fora do disco");
        std::array<uint8_t, saturnfs::kUserSectorSize> out{};
        std::copy(sectors_[lba].begin(), sectors_[lba].end(), out.begin());
        return out;
    }
    uint32_t sector_count() const override { return uint32_t(sectors_.size()); }

private:
    std::vector<testsupport::Sector> sectors_;
};

void put32(std::string& s, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[off + i] = char((v >> (8 * (3 - i))) & 0xFF);
}

// Corpo pseudoaleatório (determinístico) de 'bytes' bytes.
std::string body_of(size_t bytes, uint32_t seed = 1) {
    std::string b(bytes, 0);
    for (auto& c : b) { seed = seed * 1664525u + 1013904223u; c = char(seed >> 24); }
    return b;
}

std::string dvi_file(const std::string& body, uint32_t loop = 0xFFFFFFFF, uint32_t idx_l = 5, uint32_t idx_r = 9) {
    std::string f(0x800, 0);
    f[0] = 'D'; f[1] = 'V'; f[2] = 'I'; f[3] = '.';
    put32(f, 4, 0x800);
    put32(f, 8, uint32_t(body.size()));
    put32(f, 0xC, loop);
    put32(f, 0x10, 1000u << 16);
    put32(f, 0x14, idx_l);
    put32(f, 0x20, uint32_t(uint16_t(-500)) << 16);
    put32(f, 0x24, idx_r);
    return f + body;
}

struct Disc {
    MemoryReader reader;
    explicit Disc(testsupport::SyntheticIso&& iso) : reader(iso.sectors()) {}
};

std::vector<int16_t> read_all(TrackStream& s, size_t chunk_samples, size_t limit) {
    std::vector<int16_t> out, buf(chunk_samples);
    while (out.size() < limit) {
        const size_t n = s.read(buf);
        if (n == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out;
}

const std::string kBody = body_of(5000);   // 625 grupos, cruza 3 setores

testsupport::SyntheticIso sample_iso() {
    testsupport::SyntheticIso iso("T-TESTE", "V1.000");
    iso.add("SD/SD01.PCM", dvi_file(kBody, 0x400));        // Estágio 06, com laço
    iso.add("SD/SD1D.PCM", dvi_file(body_of(2000, 7)));      // 29: sem local conhecido, sem laço
    iso.add("SD/SD0A.PCM", dvi_file(body_of(1600, 3), 0));   // 10: laço desde o início
    iso.add("SD/SD02.PCM", std::string(3000, 'x'));          // sem magic DVI: ignorado
    iso.add("SD/EFEITO.PCM", dvi_file(body_of(64)));         // nome fora do padrão: ignorado
    iso.add("OUTRO/SD03.PCM", dvi_file(body_of(64)));        // pasta errada: ignorado
    iso.add("LEIAME.TXT", "oi");
    return iso;
}

}  // namespace

TEST_CASE("galeria: catálogo lista só SD/SDxx.PCM com cabeçalho DVI, em ordem, com duração, laço e local", "[gallery]") {
    Disc d(sample_iso());
    const auto tracks = build_track_catalog(d.reader);
    REQUIRE(tracks.size() == 3);
    CHECK(tracks[0].number == 1);
    CHECK(tracks[1].number == 10);
    CHECK(tracks[2].number == 29);
    CHECK(tracks[0].file == "SD01.PCM");
    CHECK(tracks[0].seconds == Catch::Approx(5000.0 / 44100));
    REQUIRE(tracks[0].loop_seconds);
    CHECK(*tracks[0].loop_seconds == Catch::Approx(1024.0 / 44100));
    CHECK(tracks[1].loop_seconds == std::optional<double>(0.0));
    CHECK_FALSE(tracks[2].loop_seconds);
    CHECK(tracks[0].place.label == "Estágio 06");
    CHECK(tracks[0].place.overlays == "STAGE_06");
    CHECK_FALSE(tracks[2].place.known());
    CHECK(tracks[2].place.theme == Theme::kGeneric);
}

TEST_CASE("galeria: tabela de locais cobre exatamente as 24 faixas documentadas e dá o tema invertido aos RSTAGE", "[gallery]") {
    int known = 0;
    for (int n = 1; n <= 49; ++n) known += place_for_track(n).known();
    CHECK(known == 24);
    CHECK_FALSE(place_for_track(0x0F).known());   // SD0F: sem referência na tabela
    CHECK(place_for_track(0x17).theme == Theme::kInverted);
    CHECK(place_for_track(0x03).label == "Estágio 12");
    std::set<Theme> themes;
    for (int n = 1; n <= 49; ++n) themes.insert(place_for_track(n).theme);
    CHECK(themes.size() >= 6);
}

TEST_CASE("dvi: leitura do cabeçalho só pelo começo do arquivo e decodificação por grupos igual à completa", "[gallery][dvi]") {
    const auto file = dvi_file(kBody, 0x400);
    const std::vector<uint8_t> bytes(file.begin(), file.end());
    const auto full = audio::parse_dvi_header(bytes);
    const auto head = audio::parse_dvi_header(std::span<const uint8_t>(bytes).first(0x800), bytes.size());
    CHECK(head.body_size == full.body_size);
    CHECK(head.loop_start_frames == full.loop_start_frames);
    CHECK_THROWS(audio::parse_dvi_header(std::span<const uint8_t>(bytes).first(0x800), bytes.size() + 1));

    const auto expected = audio::decode_dvi(std::span<const uint8_t>(bytes).subspan(0x800), full);
    audio::DviChannelState state[2] = {full.channel[0], full.channel[1]};
    std::vector<int16_t> out(expected.size());
    const auto body = std::span<const uint8_t>(bytes).subspan(0x800);
    audio::decode_dvi_groups(body.first(800), state, out.data());                       // em dois pedaços
    audio::decode_dvi_groups(body.subspan(800), state, out.data() + 800 * 2);
    CHECK(out == expected);
}

TEST_CASE("streaming: sem laço entrega exatamente a decodificação completa, em qualquer tamanho de pedaço", "[gallery]") {
    Disc d(sample_iso());
    const auto tracks = build_track_catalog(d.reader);
    const TrackInfo& t = tracks[2];   // SD1D, sem laço
    const std::string file = dvi_file(body_of(2000, 7));
    const std::vector<uint8_t> bytes(file.begin(), file.end());
    const auto expected = audio::decode_dvi(std::span<const uint8_t>(bytes).subspan(0x800), audio::parse_dvi_header(bytes));

    for (size_t chunk : {size_t(16), size_t(48), size_t(1000), size_t(8192)}) {
        TrackStream s(d.reader, t);
        const auto pcm = read_all(s, chunk, 1u << 20);
        CHECK(pcm == expected);
        CHECK(s.finished());
        CHECK(s.decoded_frames() == 2000);
        std::vector<int16_t> more(64);
        CHECK(s.read(more) == 0);
    }
}

TEST_CASE("streaming: com laço volta ao ponto +0x0C sem emenda e continua para sempre", "[gallery]") {
    Disc d(sample_iso());
    const auto tracks = build_track_catalog(d.reader);
    const TrackInfo& t = tracks[0];   // 5000 quadros, laço em 1024
    const std::string file = dvi_file(kBody, 0x400);
    const std::vector<uint8_t> bytes(file.begin(), file.end());
    const auto pcm = audio::decode_dvi(std::span<const uint8_t>(bytes).subspan(0x800), audio::parse_dvi_header(bytes));

    TrackStream s(d.reader, t);
    const auto got = read_all(s, 800, 2 * (5000 + 3 * (5000 - 1024)));   // primeira passada + 3 voltas
    REQUIRE(got.size() >= 2 * (5000 + 2 * (5000 - 1024)));
    CHECK(std::equal(pcm.begin(), pcm.end(), got.begin()));
    // depois do fim, repete [1024, 5000) quadro a quadro
    for (size_t i = 0; i < 2 * (5000 - 1024) * 2; ++i) REQUIRE(got[2 * 5000 + i] == pcm[2 * 1024 + i % (2 * (5000 - 1024))]);
    CHECK_FALSE(s.finished());
}

TEST_CASE("streaming: laço no início (0) repete a faixa inteira; laço além do fim é ignorado", "[gallery]") {
    Disc d(sample_iso());
    const auto tracks = build_track_catalog(d.reader);
    TrackStream s(d.reader, tracks[1]);   // 1600 quadros, laço 0
    const auto got = read_all(s, 512, 2 * 1600 * 2);
    REQUIRE(got.size() >= 2 * 1600 * 2);
    for (size_t i = 0; i < 2 * 1600; ++i) REQUIRE(got[i] == got[2 * 1600 + i]);

    TrackInfo bad = tracks[2];
    bad.header.loop_start_frames = 99999;   // fora da faixa
    TrackStream no_loop(d.reader, bad);
    CHECK(read_all(no_loop, 256, 1u << 20).size() == 2 * 2000);
}

TEST_CASE("posição de reprodução aplica o laço", "[gallery]") {
    Disc d(sample_iso());
    const auto tracks = build_track_catalog(d.reader);
    const TrackInfo& t = tracks[0];
    CHECK(track_position_seconds(t, 441) == Catch::Approx(0.01));
    CHECK(track_position_seconds(t, 4999) == Catch::Approx(4999.0 / 44100));
    CHECK(track_position_seconds(t, 5000) == Catch::Approx(1024.0 / 44100));   // chegou ao fim: está no ponto de laço
    CHECK(track_position_seconds(t, 5000 + 100) == Catch::Approx((1024 + 100) / 44100.0));
    CHECK(track_position_seconds(t, 5000 + (5000 - 1024) + 8) == Catch::Approx((1024 + 8) / 44100.0));
    CHECK(track_position_seconds(tracks[2], 999999) == Catch::Approx(2000.0 / 44100));   // sem laço: trava no fim
}

TEST_CASE("espectro: um seno de 1 kHz acende a banda certa e silêncio dá zero", "[gallery][scenes]") {
    std::vector<int16_t> pcm(2 * 2048);
    for (size_t i = 0; i < 2048; ++i) pcm[2 * i] = pcm[2 * i + 1] = int16_t(12000 * std::sin(2 * 3.14159265 * 1000.0 * double(i) / 44100));
    const Spectrum s = compute_spectrum(pcm, 44100);
    const int peak = int(std::max_element(s.begin(), s.end()) - s.begin());
    const float expected_band = std::log(1000.0f / 60.0f) / std::log(200.0f) * (kSpectrumBands - 1);
    CHECK(std::abs(peak - expected_band) <= 1.0f);
    CHECK(s[peak] > 0.6f);
    CHECK(s[0] < s[peak] * 0.5f);
    CHECK(s[kSpectrumBands - 1] < s[peak] * 0.5f);

    const Spectrum silence = compute_spectrum(std::vector<int16_t>(4096, 0), 44100);
    for (float v : silence) CHECK(v == 0.0f);
    for (float v : compute_spectrum(std::vector<int16_t>(10), 44100)) CHECK(v == 0.0f);   // curto demais
}

TEST_CASE("cenários: determinísticos, opacos, animados e diferentes por tema", "[gallery][scenes]") {
    std::set<std::vector<uint32_t>> images;
    for (int i = 0; i < kThemeCount; ++i) {
        const SceneParams p{Theme(i), 7, 3.25, 0.4f};
        video::Frame a(320, 224), b(320, 224), later(320, 224);
        render_scene(a, p);
        render_scene(b, p);
        CHECK(a.pixels == b.pixels);                                       // mesmo tempo => mesmo quadro
        CHECK(std::all_of(a.pixels.begin(), a.pixels.end(), [](uint32_t px) { return (px >> 24) == 255; }));
        render_scene(later, {p.theme, p.seed, 3.75, p.energy});
        CHECK(later.pixels != a.pixels);                                   // o tempo anima
        std::set<uint32_t> colors(a.pixels.begin(), a.pixels.end());
        CHECK(colors.size() > 50);                                         // não é cor chapada
        images.insert(a.pixels);
        CHECK_FALSE(std::string(theme_name(Theme(i))).empty());
    }
    CHECK(images.size() == size_t(kThemeCount));
    video::Frame x(320, 224), y(320, 224);
    render_scene(x, {Theme::kGeneric, 1, 1, 0});
    render_scene(y, {Theme::kGeneric, 2, 1, 0});
    CHECK(x.pixels != y.pixels);   // a semente varia o genérico
}

TEST_CASE("espectro desenhado: barras sobem com o nível", "[gallery][scenes]") {
    video::Frame quiet(320, 224), loud(320, 224);
    Spectrum high{};
    high.fill(0.9f);
    draw_spectrum(quiet, 10, 100, 300, 40, Spectrum{}, Theme::kCastle, 1.0);
    draw_spectrum(loud, 10, 100, 300, 40, high, Theme::kCastle, 1.0);
    const auto lit = [](const video::Frame& f) { return std::count_if(f.pixels.begin(), f.pixels.end(), [](uint32_t px) { return (px >> 24) == 255; }); };
    CHECK(lit(loud) > 3 * lit(quiet));
    CHECK(lit(quiet) > 0);   // parado, ainda mexe de leve
}

TEST_CASE("galeria: seleção, rolagem e troca de faixa com volta", "[gallery]") {
    testsupport::SyntheticIso iso("T-TESTE", "V1.000");
    for (int n = 1; n <= 12; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "SD/SD%02X.PCM", n);
        iso.add(name, dvi_file(body_of(256, n)));
    }
    Disc d(std::move(iso));
    Gallery g(d.reader);
    g.load();
    REQUIRE(g.tracks().size() == 12);
    g.move(-1);
    CHECK(g.selected() == 0);
    g.move(+20);
    CHECK(g.selected() == 11);
    CHECK(g.scroll() == 12 - Gallery::kGalleryRows);
    g.step_track(+1);   // volta ao início
    CHECK(g.selected() == 0);
    CHECK(g.scroll() == 0);
    g.step_track(-1);
    CHECK(g.selected() == 11);
    g.select(4);
    CHECK(g.selected() == 4);
    CHECK_FALSE(g.playing());
}

TEST_CASE("galeria: tocar enche a fila até o alvo, trocar de faixa descarta o resto e parar silencia", "[gallery]") {
    Disc d(sample_iso());
    Gallery g(d.reader);
    g.load();
    g.select(0);
    g.toggle_play();
    REQUIRE(g.playing());
    CHECK(g.playing_index() == 0);
    CHECK(g.take_flush());
    CHECK_FALSE(g.take_flush());

    size_t pushed = 0;
    const auto sink = [&pushed](std::span<const int16_t> pcm) { pushed += pcm.size(); };
    g.pump(0, sink);
    CHECK(pushed / 2 >= size_t(Gallery::kTargetQueueFrames));
    CHECK(pushed / 2 < size_t(Gallery::kTargetQueueFrames + 8 * Gallery::kChunkFrames));
    const size_t before = pushed;
    g.pump(Gallery::kTargetQueueFrames * 4, sink);   // já cheia: não faz nada
    CHECK(pushed == before);
    g.advance(0.05);
    CHECK(*std::max_element(g.spectrum().begin(), g.spectrum().end()) > 0.0f);   // o espectro reage

    g.step_track(+1);   // tocando: toca a seguinte
    CHECK(g.playing_index() == 1);
    CHECK(g.take_flush());
    g.toggle_play();    // mesma faixa: para
    CHECK_FALSE(g.playing());
    CHECK(g.take_flush());
    for (int i = 0; i < 30; ++i) g.advance(0.1);
    for (float v : g.spectrum()) CHECK(v == 0.0f);   // as barras caem a zero
    g.pump(0, sink);   // parado: não decodifica
    CHECK(g.position_seconds() == 0);
}

TEST_CASE("galeria: faixa sem laço para sozinha quando acaba e a fila esvazia; erro de leitura vira mensagem", "[gallery]") {
    Disc d(sample_iso());
    Gallery g(d.reader);
    g.load();
    g.select(2);   // SD1D, sem laço, 2000 quadros
    g.toggle_play();
    size_t pushed = 0;
    g.pump(0, [&pushed](std::span<const int16_t> pcm) { pushed += pcm.size(); });
    CHECK(pushed == 2 * 2000);
    CHECK(g.playing());                                  // ainda há som na fila
    g.pump(1000 * 4, [](std::span<const int16_t>) {});
    CHECK(g.playing());
    g.pump(0, [](std::span<const int16_t>) {});          // fila vazia
    CHECK_FALSE(g.playing());

    // disco que perde os setores depois do catálogo
    class Flaky : public saturnfs::SectorReader {
    public:
        explicit Flaky(const MemoryReader& inner) : inner_(inner) {}
        std::array<uint8_t, saturnfs::kUserSectorSize> read_user_sector(uint32_t lba) const override {
            if (broken) throw std::runtime_error("setor ilegível");
            return inner_.read_user_sector(lba);
        }
        uint32_t sector_count() const override { return inner_.sector_count(); }
        bool broken = false;
    private:
        const MemoryReader& inner_;
    } flaky(d.reader);
    Gallery h(flaky);
    h.load();
    h.select(0);
    h.toggle_play();
    flaky.broken = true;
    h.pump(0, [](std::span<const int16_t>) {});
    CHECK_FALSE(h.playing());
    CHECK(h.error().find("setor ilegível") != std::string::npos);
}

TEST_CASE("menu: a galeria só aparece com o host dela e a navegação vai para a música", "[gallery][menu]") {
    Disc d(sample_iso());
    Gallery gallery(d.reader);
    Config config;
    {
        Menu plain(config, MenuHost{});
        plain.open();
        for (const auto& row : plain.rows()) CHECK(row.label != "Galeria da trilha sonora");
        CHECK_FALSE(plain.open_path({"Galeria da trilha sonora"}));
    }
    MenuHost host;
    host.gallery = &gallery;
    Menu menu(config, host);
    REQUIRE(menu.open_path({"Galeria da trilha sonora"}));
    CHECK(menu.in_gallery());
    CHECK(menu.title() == "Menu > Galeria da trilha sonora");
    REQUIRE(gallery.loaded());

    menu.handle(Nav::kDown);
    CHECK(gallery.selected() == 1);
    menu.handle(Nav::kRight);   // não está tocando: só troca a seleção
    CHECK(gallery.selected() == 2);
    CHECK_FALSE(gallery.playing());
    menu.handle(Nav::kLeft);
    menu.handle(Nav::kConfirm);
    CHECK(gallery.playing_index() == 1);
    menu.handle(Nav::kRight);   // tocando: segue tocando a nova
    CHECK(gallery.playing_index() == 2);

    video::Frame overlay(kOverlayW, kOverlayH);
    draw_menu(menu, overlay);
    CHECK(std::all_of(overlay.pixels.begin(), overlay.pixels.end(), [](uint32_t px) { return (px >> 24) == 255; }));

    const int row = menu.row_at(20, 24 + 2 + 12 * 1 + 3);   // segunda linha da lista
    CHECK(row == gallery.scroll() + 1);
    menu.click(row);            // clique numa faixa que não está tocando: toca
    CHECK(gallery.playing_index() == row);
    menu.click(row);            // na que toca: para
    CHECK_FALSE(gallery.playing());
    CHECK(menu.row_at(300, 100) == -1);
    menu.hover(-1);             // fora da lista não mexe na seleção
    CHECK(gallery.selected() == row);

    menu.handle(Nav::kConfirm);
    REQUIRE(gallery.playing());
    menu.handle(Nav::kBack);    // sai da página: a música para e o menu volta à raiz
    CHECK_FALSE(gallery.playing());
    CHECK_FALSE(menu.in_gallery());
    CHECK(menu.is_open());
    CHECK(menu.title() == "Menu");

    REQUIRE(menu.open_path({"Galeria da trilha sonora"}));
    gallery.toggle_play();
    REQUIRE(gallery.playing());
    menu.close();               // fechar o menu também silencia
    CHECK_FALSE(gallery.playing());
}
