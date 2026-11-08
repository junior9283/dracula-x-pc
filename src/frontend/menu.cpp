#include "frontend/menu.hpp"

#include <algorithm>
#include <stdexcept>

#include "frontend/font.hpp"

namespace frontend {

using video::rgba;

Menu::Menu(Config& config, MenuHost host) : config_(config), host_(std::move(host)) { open(); open_ = false; }

void Menu::open() {
    leave_gallery();
    stack_.clear();
    capture_ = {};
    message_.clear();
    message_error_ = false;
    refresh_slots();
    push(build_root());
    open_ = true;
}

bool Menu::open_path(const std::vector<std::string>& labels) {
    open();
    for (const auto& label : labels) {
        const auto& items = page().items;
        const auto it = std::find_if(items.begin(), items.end(), [&](const Item& i) { return i.label == label; });
        if (it == items.end() || !it->activate) return false;
        select(int(it - items.begin()));
        confirm();
    }
    return true;
}

void Menu::push(Page page) {
    message_.clear();
    stack_.push_back(std::move(page));
    fix_selection(this->page(), +1);
    ensure_visible(this->page());
}

void Menu::leave_gallery() {
    if (!stack_.empty() && page().gallery && host_.gallery) host_.gallery->stop();
}

void Menu::pop() {
    message_.clear();
    leave_gallery();
    if (stack_.size() > 1) stack_.pop_back();
    else close();
}

void Menu::fix_selection(Page& p, int direction) const {
    const int n = int(p.items.size());
    for (int i = 0; i < n; ++i) {
        const int candidate = ((p.selected + i * direction) % n + n) % n;
        if (p.items[candidate].selectable()) { p.selected = candidate; return; }
    }
}

void Menu::ensure_visible(Page& p) const {
    if (p.selected < p.scroll) p.scroll = p.selected;
    if (p.selected >= p.scroll + kMenuVisibleRows) p.scroll = p.selected - kMenuVisibleRows + 1;
    p.scroll = std::clamp(p.scroll, 0, std::max(0, int(p.items.size()) - kMenuVisibleRows));
}

void Menu::select(int row) {
    Page& p = page();
    if (p.gallery) {
        if (row >= 0) host_.gallery->select(row);
        return;
    }
    if (row < 0 || row >= int(p.items.size()) || !p.items[row].selectable()) return;
    p.selected = row;
    ensure_visible(p);
}

void Menu::move(int step) {
    Page& p = page();
    if (p.gallery) { host_.gallery->move(step); return; }
    const int n = int(p.items.size());
    if (std::abs(step) == 1) {
        p.selected = ((p.selected + step) % n + n) % n;
        fix_selection(p, step);
    } else {   // página: anda sem dar a volta e cai no item selecionável mais próximo
        p.selected = std::clamp(p.selected + step, 0, n - 1);
        fix_selection(p, step > 0 ? -1 : +1);
    }
    ensure_visible(p);
}

void Menu::confirm() {
    const Item& item = page().items[page().selected];
    if (item.activate) {
        const auto fn = item.activate;   // cópia: a ação pode empilhar/desempilhar páginas e destruir este item
        fn();
    } else if (item.adjust && item.confirm_adjusts) {
        adjust(+1);
    }
}

void Menu::adjust(int direction) {
    const Item& item = page().items[page().selected];
    if (!item.adjust) return;
    const auto fn = item.adjust;
    fn(direction);
    changed_ = true;
}

void Menu::handle(Nav nav) {
    if (!open_) return;
    if (capture_.active) {
        if (nav == Nav::kBack) cancel_capture();
        return;
    }
    if (page().gallery) { handle_gallery(nav); return; }
    switch (nav) {
        case Nav::kUp: move(-1); break;
        case Nav::kDown: move(+1); break;
        case Nav::kPageUp: move(-kMenuVisibleRows); break;
        case Nav::kPageDown: move(+kMenuVisibleRows); break;
        case Nav::kLeft: adjust(-1); break;
        case Nav::kRight: adjust(+1); break;
        case Nav::kConfirm: confirm(); break;
        case Nav::kBack: pop(); break;
    }
}

void Menu::handle_gallery(Nav nav) {
    Gallery& g = *host_.gallery;
    switch (nav) {
        case Nav::kUp: g.move(-1); break;
        case Nav::kDown: g.move(+1); break;
        case Nav::kPageUp: g.move(-Gallery::kGalleryRows); break;
        case Nav::kPageDown: g.move(+Gallery::kGalleryRows); break;
        case Nav::kLeft: g.step_track(-1); break;
        case Nav::kRight: g.step_track(+1); break;
        case Nav::kConfirm: g.toggle_play(); break;
        case Nav::kBack: pop(); break;
    }
}

void Menu::hover(int row) {
    if (open_ && !capture_.active) select(row);
}

void Menu::click(int row) {
    if (!open_) return;
    if (capture_.active) { cancel_capture(); return; }
    if (page().gallery) {
        if (row < 0) return;
        Gallery& g = *host_.gallery;   // clique toca a faixa; clique na que já toca, para
        g.select(row);
        if (g.playing_index() == row) g.stop();
        else g.play(row);
        return;
    }
    if (row < 0 || row >= int(page().items.size()) || !page().items[row].selectable()) return;
    select(row);
    confirm();
}

void Menu::wheel(int direction, int row) {
    if (!open_ || capture_.active || direction == 0) return;
    if (page().gallery) { host_.gallery->move(direction > 0 ? -1 : +1); return; }
    select(row);
    if (page().items[page().selected].adjust && row >= 0) adjust(direction > 0 ? +1 : -1);
    else move(direction > 0 ? -1 : +1);   // roda para cima = sobe
}

int Menu::row_at(int x, int y) const {
    if (page().gallery) return gallery_row_at(*host_.gallery, x, y);
    if (x < 6 || x >= kOverlayW - 6 || y < kMenuRowsTop || y >= kMenuRowsTop + kMenuVisibleRows * kMenuRowHeight) return -1;
    const int row = page().scroll + (y - kMenuRowsTop) / kMenuRowHeight;
    return row < int(page().items.size()) ? row : -1;
}

std::string Menu::title() const {
    std::string out;
    for (const auto& p : stack_) out += (out.empty() ? "" : " > ") + p.title;
    return out;
}

std::vector<MenuRow> Menu::rows() const {
    std::vector<MenuRow> out;
    const Page& p = page();
    for (int i = 0; i < int(p.items.size()); ++i) {
        const Item& item = p.items[i];
        out.push_back({item.label, item.value ? item.value() : std::string(), item.selectable(), bool(item.adjust), i == p.selected});
    }
    return out;
}

// ---- Remapeamento ----

void Menu::cancel_capture() {
    if (capture_.active) message_.clear();
    capture_ = {};
}

void Menu::finish_capture(int code) {
    auto& slots = capture_.keyboard ? config_.bindings.keys : config_.bindings.pad;
    const int button = capture_.button;
    for (int other = 0; other < kButtonCount; ++other)
        if (other != button && slots[other] == code) slots[other] = slots[button];   // trocam de lugar: nada fica duplicado
    slots[button] = code;
    capture_ = {};
    message_.clear();
    changed_ = true;
}

void Menu::capture_key(int scancode) {
    if (!capture_.active) return;
    if (scancode == kScancodeEscape) cancel_capture();
    else if (capture_.keyboard && valid_key_code(scancode)) finish_capture(scancode);
}

void Menu::capture_pad(int code) {
    if (capture_.active && !capture_.keyboard && valid_pad_code(code)) finish_capture(code);
}

// ---- Construção das páginas ----

Menu::Item Menu::toggle(std::string label, bool& field, std::string off, std::string on) {
    Item item;
    item.label = std::move(label);
    item.value = [&field, off = std::move(off), on = std::move(on)] { return field ? on : off; };
    item.adjust = [&field](int) { field = !field; };
    item.confirm_adjusts = true;
    return item;
}

Menu::Item Menu::number(std::string label, int& field, int lo, int hi, int step, std::string suffix) {
    Item item;
    item.label = std::move(label);
    item.value = [&field, suffix = std::move(suffix)] { return std::to_string(field) + suffix; };
    item.adjust = [&field, lo, hi, step](int dir) { field = std::clamp(field + dir * step, lo, hi); };
    return item;
}

template <class T>
Menu::Item Menu::choice(std::string label, T& field, std::vector<std::pair<T, std::string>> options) {
    Item item;
    item.label = std::move(label);
    item.value = [&field, options] {
        for (const auto& [value, name] : options)
            if (value == field) return name;
        return options.front().second;
    };
    item.adjust = [&field, options = std::move(options)](int dir) {
        const int n = int(options.size());
        int index = 0;
        for (int i = 0; i < n; ++i)
            if (options[i].first == field) index = i;
        field = options[((index + dir) % n + n) % n].first;
    };
    item.confirm_adjusts = true;
    return item;
}

Menu::Item Menu::action(std::string label, std::function<void()> fn) {
    Item item;
    item.label = std::move(label);
    item.activate = std::move(fn);
    return item;
}

Menu::Item Menu::submenu(std::string label, std::function<Page()> build) {
    return action(std::move(label), [this, build = std::move(build)] { push(build()); });
}

Menu::Item Menu::back_item() { return action("Voltar", [this] { pop(); }); }

Menu::Item Menu::binding(int button, bool keyboard) {
    Item item;
    item.label = button_label(button);
    item.value = [this, button, keyboard] {
        if (capture_.active && capture_.button == button && capture_.keyboard == keyboard)
            return std::string(keyboard ? "pressione a tecla..." : "pressione o botão...");
        const int code = keyboard ? config_.bindings.keys[button] : config_.bindings.pad[button];
        if (keyboard) return code == kUnbound ? std::string("(nenhuma)") : host_.key_name ? host_.key_name(code) : "Tecla " + std::to_string(code);
        return pad_input_name(code);
    };
    item.activate = [this, button, keyboard] {
        capture_ = {true, keyboard, button};
        set_message(std::string(keyboard ? "Pressione a tecla" : "Pressione o botão") + " para " + button_label(button) +
                    " (Esc cancela)");
    };
    return item;
}

Menu::Page Menu::build_root() {
    Page p{"Menu", {}};
    p.items.push_back(action("Continuar", [this] { close(); }));
    p.items.push_back(submenu("Save states", [this] { return build_states(); }));
    p.items.push_back(submenu("Vídeo", [this] { return build_video(); }));
    p.items.push_back(submenu("Áudio", [this] { return build_audio(); }));
    if (host_.gallery) p.items.push_back(submenu("Galeria da trilha sonora", [this] { return build_gallery(); }));
    p.items.push_back(submenu("Controles", [this] { return build_controls(); }));
    p.items.push_back(submenu("Sair do jogo", [this] { return build_quit(); }));
    return p;
}

void Menu::refresh_slots() {
    for (int i = 0; i < kSlotCount; ++i) slot_text_[i] = host_.slot_text ? host_.slot_text(i) : std::string("vazio");
}

Menu::Page Menu::build_states() {
    refresh_slots();
    Page p{"Save states", {}};
    for (int i = 0; i < kSlotCount; ++i) {
        Item item = action("Slot " + std::to_string(i + 1), [this, i] {
            config_.slot = i;
            changed_ = true;
            push(build_slot(i));
        });
        item.value = [this, i] { return (config_.slot == i ? "* " : "") + slot_text_[i]; };
        p.items.push_back(std::move(item));
    }
    p.items.push_back(back_item());
    p.selected = config_.slot;
    return p;
}

Menu::Page Menu::build_slot(int slot) {
    Page p{"Slot " + std::to_string(slot + 1), {}};
    Item info;
    info.label = "Gravado em:";
    info.value = [this, slot] { return slot_text_[slot]; };
    p.items.push_back(std::move(info));
    p.items.push_back(action("Salvar neste slot", [this, slot] { run_save_load(true, slot); }));
    p.items.push_back(action("Carregar deste slot", [this, slot] { run_save_load(false, slot); }));
    p.items.push_back(back_item());
    return p;
}

void Menu::run_save_load(bool save, int slot) {
    const auto& fn = save ? host_.save_state : host_.load_state;
    try {
        if (!fn) throw std::runtime_error("indisponível");
        fn(slot);
    } catch (const std::exception& e) {
        set_message(std::string("Erro: ") + e.what(), true);
        refresh_slots();
        return;
    }
    refresh_slots();
    if (save) {
        set_message("Estado salvo no slot " + std::to_string(slot + 1));
    } else {
        close();
        set_message("Estado carregado do slot " + std::to_string(slot + 1));
    }
}

Menu::Page Menu::build_video() {
    Page p{"Vídeo", {}};
    p.items.push_back(toggle("Modo da janela", config_.fullscreen, "Janela", "Tela cheia sem borda"));
    p.items.push_back(toggle("Escala inteira", config_.integer_scale, "Desligada", "Ligada"));
    p.items.push_back(choice<Aspect>("Proporção", config_.aspect,
                                     {{Aspect::kOriginal, "Pixel original"}, {Aspect::k4x3, "4:3"}, {Aspect::kStretch, "Esticar"}}));
    p.items.push_back(choice<Filter>("Filtro", config_.filter, {{Filter::kSharp, "Nítido"}, {Filter::kSmooth, "Suave"}}));
    p.items.push_back(toggle("Scanlines", config_.scanlines, "Desligadas", "Ligadas"));
    p.items.push_back(toggle("Vsync", config_.vsync, "Desligado", "Ligado"));
    p.items.push_back(number("Escala inicial da janela", config_.window_scale, 1, 8, 1, "x"));
    p.items.push_back(back_item());
    return p;
}

Menu::Page Menu::build_audio() {
    Page p{"Áudio", {}};
    p.items.push_back(number("Volume geral", config_.volume, 0, 100, 5, "%"));
    p.items.push_back(toggle("Mudo", config_.mute, "Não", "Sim"));
    p.items.push_back(choice<Latency>("Latência", config_.latency,
                                      {{Latency::kLow, "Baixa"}, {Latency::kMedium, "Média"}, {Latency::kHigh, "Alta"}}));
    p.items.push_back(back_item());
    return p;
}

Menu::Page Menu::build_controls() {
    Page p{"Controles", {}};
    p.items.push_back(submenu("Teclado", [this] { return build_bindings(true); }));
    p.items.push_back(submenu("Controle", [this] { return build_bindings(false); }));
    p.items.push_back(number("Zona morta do analógico", config_.deadzone, 5, 60, 5, "%"));
    p.items.push_back(action("Restaurar tudo ao padrão", [this] {
        const Config defaults;
        config_.bindings = defaults.bindings;
        config_.deadzone = defaults.deadzone;
        changed_ = true;
        set_message("Controles restaurados ao padrão");
    }));
    p.items.push_back(back_item());
    return p;
}

Menu::Page Menu::build_bindings(bool keyboard) {
    Page p{keyboard ? "Teclado" : "Controle", {}};
    for (int b = 0; b < kButtonCount; ++b) p.items.push_back(binding(b, keyboard));
    p.items.push_back(action("Restaurar padrão", [this, keyboard] {
        (keyboard ? config_.bindings.keys : config_.bindings.pad) = (keyboard ? default_bindings().keys : default_bindings().pad);
        changed_ = true;
        set_message(keyboard ? "Teclado restaurado ao padrão" : "Controle restaurado ao padrão");
    }));
    p.items.push_back(back_item());
    return p;
}

Menu::Page Menu::build_gallery() {
    Page p{"Galeria da trilha sonora", {}};
    p.gallery = true;
    host_.gallery->load();
    return p;
}

Menu::Page Menu::build_quit() {
    Page p{"Sair do jogo", {}};
    Item info;
    info.label = "Sair do jogo agora?";
    p.items.push_back(std::move(info));
    p.items.push_back(action("Não, voltar", [this] { pop(); }));
    p.items.push_back(action("Sim, sair do jogo", [this] {
        if (host_.quit) host_.quit();
    }));
    p.selected = 1;
    return p;
}

// ---- Renderização ----

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFFu;

// Quebra em linhas de até 'columns' caracteres (UTF-8), no máximo 'max_lines'; o excesso vira "...".
std::vector<std::string> wrap_text(const std::string& text, size_t columns, size_t max_lines) {
    std::vector<std::string> lines;
    size_t start = 0;
    const auto chars_between = [&](size_t a, size_t b) {
        size_t n = 0;
        for (size_t i = a; i < b; ++i) n += (uint8_t(text[i]) & 0xC0) != 0x80;
        return n;
    };
    while (start < text.size() && lines.size() < max_lines) {
        size_t end = start, last_space = std::string::npos;
        while (end < text.size() && chars_between(start, end + 1) <= columns) {
            if (text[end] == ' ') last_space = end;
            ++end;
        }
        while (end < text.size() && (uint8_t(text[end]) & 0xC0) == 0x80) ++end;   // não corta um caractere no meio
        if (end < text.size() && last_space != std::string::npos) end = last_space;
        lines.push_back(text.substr(start, end - start));
        start = end;
        while (start < text.size() && text[start] == ' ') ++start;
    }
    if (start < text.size() && !lines.empty()) {
        auto& last = lines.back();
        while (!last.empty() && chars_between(0, last.size()) + 3 > columns) {
            do last.pop_back(); while (!last.empty() && (uint8_t(last.back()) & 0xC0) == 0x80);
        }
        last += "...";
    }
    return lines;
}

}  // namespace

void draw_menu(const Menu& menu, video::Frame& overlay) {
    if (menu.in_gallery()) { draw_gallery(*menu.gallery(), overlay); return; }
    fill_rect(overlay, 0, 0, overlay.width, overlay.height, rgba(0, 0, 0, 175));
    const uint32_t gold = rgba(235, 190, 90), rule = rgba(200, 150, 60, 150);
    fill_rect(overlay, 4, 3, kOverlayW - 8, 1, rule);
    draw_text(overlay, 8, 5, menu.title(), gold, kOverlayW - 8);
    fill_rect(overlay, 4, 19, kOverlayW - 8, 1, rule);

    const auto rows = menu.rows();
    const int first = menu.scroll(), last = std::min(int(rows.size()), first + kMenuVisibleRows);
    for (int i = first; i < last; ++i) {
        const MenuRow& row = rows[i];
        const int y = kMenuRowsTop + (i - first) * kMenuRowHeight;
        if (row.selected) fill_rect(overlay, 8, y, kOverlayW - 20, kMenuRowHeight, rgba(140, 25, 50, 230));
        const uint32_t label_color = !row.selectable ? rgba(170, 170, 170) : row.selected ? rgba(255, 235, 150) : kWhite;
        int value_x = kOverlayW - 14;
        if (!row.value.empty()) {
            const int width = text_width(row.value) + (row.adjustable ? 2 * kGlyphWidth : 0);
            value_x = kOverlayW - 14 - width;
            const uint32_t value_color = rgba(150, 215, 255);
            int x = value_x;
            if (row.adjustable) x = draw_text(overlay, x, y, "<", row.selected ? kWhite : rgba(120, 120, 120));
            x = draw_text(overlay, x, y, row.value, value_color);
            if (row.adjustable) draw_text(overlay, x, y, ">", row.selected ? kWhite : rgba(120, 120, 120));
        }
        draw_text(overlay, 14, y, row.label, label_color, value_x - 6);
    }
    if (int(rows.size()) > kMenuVisibleRows) {   // barra de rolagem
        const int track = kMenuVisibleRows * kMenuRowHeight, total = int(rows.size());
        const int thumb = std::max(8, track * kMenuVisibleRows / total), top = (track - thumb) * first / (total - kMenuVisibleRows);
        fill_rect(overlay, kOverlayW - 8, kMenuRowsTop, 2, track, rgba(255, 255, 255, 40));
        fill_rect(overlay, kOverlayW - 8, kMenuRowsTop + top, 2, thumb, gold);
    }

    const auto lines = wrap_text(menu.message(), 50, 2);
    for (size_t i = 0; i < lines.size(); ++i)
        draw_text(overlay, 8, 180 + int(i) * kGlyphHeight, lines[i], menu.message_is_error() ? rgba(255, 120, 110) : rgba(150, 230, 150));
    fill_rect(overlay, 4, 206, kOverlayW - 8, 1, rule);
    draw_text(overlay, 8, 210, menu.capturing() ? "Esc: cancelar" : "Setas/mouse: mover   Enter/clique: ok   Esc: voltar",
              rgba(190, 190, 190), kOverlayW - 8);
}

void draw_toast(video::Frame& overlay, const std::string& text, bool error) {
    const int width = std::min(text_width(text) + 12, kOverlayW - 8);
    const int x = (overlay.width - width) / 2, y = overlay.height - 22;
    fill_rect(overlay, x, y, width, 16, rgba(0, 0, 0, 200));
    draw_text(overlay, x + 6, y + 2, text, error ? rgba(255, 130, 120) : kWhite, x + width - 6 + 1);
}

void composite(video::Frame& base, const video::Frame& overlay) {
    const size_t n = std::min(base.pixels.size(), overlay.pixels.size());
    for (size_t i = 0; i < n; ++i) base.pixels[i] = blend_pixel(base.pixels[i], overlay.pixels[i]);
}

}  // namespace frontend
