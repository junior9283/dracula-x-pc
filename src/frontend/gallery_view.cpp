// Desenho da página da galeria no overlay: cenário animado ao fundo, lista de faixas, ficha da faixa e espectro.
#include <algorithm>
#include <cstdio>

#include "frontend/font.hpp"
#include "frontend/gallery.hpp"

namespace frontend {
namespace {

using video::rgba;

constexpr int kListX = 6, kListW = 160, kListY = 24, kRowH = 12, kListPad = 2;
constexpr int kInfoY = 140, kSpectrumY = 182, kSpectrumH = 20, kFooterY = 210;

std::string mmss(double seconds) {
    const int total = int(seconds + 0.5);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return buf;
}

void panel(video::Frame& f, int x, int y, int w, int h) {
    fill_rect(f, x, y, w, h, rgba(4, 2, 10, 135));
    fill_rect(f, x, y, w, 1, rgba(200, 150, 60, 150));
    fill_rect(f, x, y + h - 1, w, 1, rgba(200, 150, 60, 90));
}

// Ícones de 7x7 desenhados por linha (bits 0x40..0x01).
void icon(video::Frame& f, int x, int y, const uint8_t (&rows)[7], uint32_t color) {
    for (int r = 0; r < 7; ++r)
        for (int c = 0; c < 7; ++c)
            if (rows[r] & (0x40 >> c)) fill_rect(f, x + c, y + r, 1, 1, color);
}
constexpr uint8_t kPlayIcon[7] = {0x40, 0x60, 0x70, 0x78, 0x70, 0x60, 0x40};
constexpr uint8_t kLoopIcon[7] = {0x3C, 0x42, 0x41, 0x4F, 0x07, 0x0E, 0x38};   // seta circular

std::string place_text(const TrackInfo& t) {
    if (!t.place.known()) return "Local não identificado";
    return t.place.label + " (" + t.place.overlays + ")";
}

}  // namespace

int gallery_row_at(const Gallery& gallery, int x, int y) {
    if (x < kListX || x >= kListX + kListW || y < kListY + kListPad || y >= kListY + kListPad + Gallery::kGalleryRows * kRowH) return -1;
    const int row = gallery.scroll() + (y - kListY - kListPad) / kRowH;
    return row < int(gallery.tracks().size()) ? row : -1;
}

void draw_gallery(const Gallery& gallery, video::Frame& f) {
    const uint32_t gold = rgba(235, 190, 90), white = 0xFFFFFFFFu, cyan = rgba(150, 215, 255);
    const TrackInfo* current = gallery.current();
    const Theme theme = current ? current->place.theme : Theme::kGeneric;
    render_scene(f, {theme, uint32_t(current ? current->number : 0), gallery.time(), gallery.energy()});
    const double fade = 1.0 - gallery.scene_age() / 0.35;   // a troca de cenário nasce do preto
    if (fade > 0) fill_rect(f, 0, 0, f.width, f.height, rgba(0, 0, 0, uint8_t(255 * std::min(fade, 1.0))));

    // Título
    fill_rect(f, 0, 0, f.width, 20, rgba(4, 2, 10, 175));
    fill_rect(f, 4, 19, f.width - 8, 1, rgba(200, 150, 60, 150));
    draw_text(f, 8, 5, "Menu > Galeria da trilha sonora", gold, f.width - 8);
    if (current) {
        const std::string t = theme_name(theme);
        draw_text(f, f.width - 8 - text_width(t), 5, t, cyan);
    }

    // Lista
    panel(f, kListX, kListY, kListW, Gallery::kGalleryRows * kRowH + 2 * kListPad);
    const auto& tracks = gallery.tracks();
    if (tracks.empty()) draw_text(f, kListX + 6, kListY + 8, "Nenhuma faixa encontrada.", rgba(255, 120, 110), kListX + kListW - 4);
    const int first = gallery.scroll(), last = std::min(int(tracks.size()), first + Gallery::kGalleryRows);
    for (int i = first; i < last; ++i) {
        const TrackInfo& t = tracks[i];
        const int y = kListY + kListPad + (i - first) * kRowH;
        const bool selected = i == gallery.selected(), playing = i == gallery.playing_index();
        if (selected) fill_rect(f, kListX + 2, y, kListW - 8, kRowH, rgba(140, 25, 50, 230));
        char number[8];
        std::snprintf(number, sizeof number, "%02d", t.number);
        const uint32_t color = selected ? rgba(255, 235, 150) : t.place.known() ? white : rgba(175, 175, 175);
        if (playing) icon(f, kListX + 4, y + 2, kPlayIcon, rgba(120, 255, 150));
        draw_text(f, kListX + 14, y, number, color);
        draw_text(f, kListX + 30, y, t.place.known() ? t.place.label : "-", color, kListX + kListW - 56);
        const std::string len = mmss(t.seconds);
        draw_text(f, kListX + kListW - 12 - text_width(len) - (t.loop_seconds ? 10 : 0), y, len, cyan);
        if (t.loop_seconds) icon(f, kListX + kListW - 18, y + 2, kLoopIcon, rgba(190, 190, 190));
    }
    if (int(tracks.size()) > Gallery::kGalleryRows) {   // barra de rolagem
        const int track = Gallery::kGalleryRows * kRowH, total = int(tracks.size());
        const int thumb = std::max(8, track * Gallery::kGalleryRows / total);
        const int top = (track - thumb) * first / (total - Gallery::kGalleryRows);
        fill_rect(f, kListX + kListW - 5, kListY + kListPad, 2, track, rgba(255, 255, 255, 40));
        fill_rect(f, kListX + kListW - 5, kListY + kListPad + top, 2, thumb, gold);
    }

    // Ficha da faixa selecionada
    panel(f, kListX, kInfoY, f.width - 2 * kListX, 40);
    if (current) {
        char head[48];
        std::snprintf(head, sizeof head, "Faixa %02d  %s", current->number, current->file.c_str());
        draw_text(f, 12, kInfoY + 3, head, gold);
        const bool playing_this = gallery.playing_index() == gallery.selected();
        const std::string status = playing_this ? "Tocando" : gallery.playing() ? "Outra faixa tocando" : "Parado";
        draw_text(f, f.width - 12 - text_width(status), kInfoY + 3, status, playing_this ? rgba(120, 255, 150) : rgba(190, 190, 190));
        draw_text(f, 12, kInfoY + 15, gallery.error().empty() ? place_text(*current) : gallery.error(), gallery.error().empty() ? cyan : rgba(255, 120, 110),
                  f.width - 12);
        std::string meta = "Duração " + mmss(current->seconds);
        meta += current->loop_seconds ? "   laço em " + mmss(*current->loop_seconds) : "   sem laço";
        draw_text(f, 12, kInfoY + 27, meta, white, f.width - 96);
        if (playing_this) {
            const std::string pos = mmss(gallery.position_seconds()) + " / " + mmss(current->seconds);
            draw_text(f, f.width - 12 - text_width(pos), kInfoY + 27, pos, white);
            const int bar_w = f.width - 2 * kListX - 8, filled = int(bar_w * std::min(1.0, gallery.position_seconds() / std::max(current->seconds, 1.0)));
            fill_rect(f, kListX + 4, kInfoY + 38, bar_w, 1, rgba(255, 255, 255, 50));
            fill_rect(f, kListX + 4, kInfoY + 38, filled, 1, gold);
        }
    }

    draw_spectrum(f, kListX + 2, kSpectrumY, f.width - 2 * kListX - 4, kSpectrumH, gallery.spectrum(), theme, gallery.time());

    fill_rect(f, 0, kFooterY - 4, f.width, f.height - kFooterY + 4, rgba(4, 2, 10, 190));
    fill_rect(f, 4, kFooterY - 4, f.width - 8, 1, rgba(200, 150, 60, 150));
    draw_text(f, 8, kFooterY, "Enter: tocar/parar   Esq/dir: trocar   Esc: voltar", rgba(190, 190, 190), f.width - 8);
}

}  // namespace frontend
