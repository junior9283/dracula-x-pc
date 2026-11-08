// Modelo do menu do port (árvore de páginas, navegação, ajuste de valores, remapeamento) e sua renderização em software.
// Não depende do SDL: a camada do SDL só traduz eventos em chamadas e desenha o quadro resultante.
#pragma once

#include <array>
#include <functional>
#include <utility>
#include <string>
#include <vector>

#include "frontend/config.hpp"
#include "frontend/gallery.hpp"
#include "video/frame.hpp"

namespace frontend {

enum class Nav { kUp, kDown, kLeft, kRight, kConfirm, kBack, kPageUp, kPageDown };

// O que o menu pede ao aplicativo (todos opcionais; save/load podem lançar exceção, que vira mensagem no menu).
struct MenuHost {
    std::function<std::string(int)> slot_text;            // descrição do slot (data/hora ou "vazio")
    std::function<void(int)> save_state, load_state;      // slot 0..7
    std::function<void()> quit;
    std::function<std::string(int)> key_name;             // nome de uma tecla (scancode)
    Gallery* gallery = nullptr;                           // galeria da trilha sonora (sem ela, a página não aparece no menu)
};

// Geometria da tela do menu (em pixels do overlay de 320x224).
inline constexpr int kOverlayW = 320, kOverlayH = 224;
inline constexpr int kMenuRowsTop = 22, kMenuRowHeight = 12, kMenuVisibleRows = 13;

struct MenuRow {
    std::string label, value;
    bool selectable = false;
    bool adjustable = false;   // mostra "< valor >"
    bool selected = false;
};

class Menu {
public:
    Menu(Config& config, MenuHost host);
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    void open();                      // abre na raiz
    void close() { leave_gallery(); open_ = false; cancel_capture(); }
    bool is_open() const { return open_; }
    bool open_path(const std::vector<std::string>& labels);   // abre e entra nas páginas com esses rótulos (diagnóstico/testes)

    void handle(Nav nav);
    void hover(int row);              // mouse sobre a linha absoluta (seleciona se der)
    void click(int row);              // clique esquerdo
    void wheel(int direction, int row);   // roda do mouse: ajusta o valor da linha, ou rola a lista
    int row_at(int x, int y) const;   // linha absoluta sob o ponto do overlay, ou -1
    void select(int row);

    // Remapeamento: enquanto capturando, o aplicativo manda a próxima tecla/botão para cá (Esc do teclado cancela).
    bool capturing() const { return capture_.active; }
    void capture_key(int scancode);
    void capture_pad(int code);
    void cancel_capture();

    // Página da galeria: o menu repassa a navegação e o mouse à Gallery e o desenho fica por conta de draw_gallery.
    bool in_gallery() const { return open_ && page().gallery; }
    Gallery* gallery() const { return host_.gallery; }

    std::string title() const;
    std::vector<MenuRow> rows() const;
    int selected() const { return page().selected; }
    int scroll() const { return page().scroll; }
    int row_count() const { return int(page().items.size()); }
    const std::string& message() const { return message_; }
    bool message_is_error() const { return message_error_; }
    void set_message(std::string text, bool error = false) { message_ = std::move(text); message_error_ = error; }
    bool take_changed() { return std::exchange(changed_, false); }   // alguma opção mudou desde a última chamada

private:
    struct Item {
        std::string label;
        std::function<std::string()> value;
        std::function<void(int)> adjust;   // direção -1/+1
        std::function<void()> activate;
        bool confirm_adjusts = false;      // Enter/clique avança o valor (alternar/ciclar)
        bool selectable() const { return adjust || activate; }
    };
    struct Page {
        std::string title;
        std::vector<Item> items;
        int selected = 0, scroll = 0;
        bool gallery = false;   // página especial: não usa 'items'
    };
    struct Capture { bool active = false; bool keyboard = true; int button = 0; };

    Page& page() { return stack_.back(); }
    const Page& page() const { return stack_.back(); }
    void push(Page page);
    void pop();
    void move(int step);
    void confirm();
    void adjust(int direction);
    void fix_selection(Page& page, int direction) const;
    void ensure_visible(Page& page) const;
    void run_save_load(bool save, int slot);
    void refresh_slots();
    void leave_gallery();      // para a música ao sair da página (ou fechar o menu)
    void handle_gallery(Nav nav);
    void finish_capture(int code);

    Item toggle(std::string label, bool& field, std::string off, std::string on);
    Item number(std::string label, int& field, int lo, int hi, int step, std::string suffix);
    template <class T>
    Item choice(std::string label, T& field, std::vector<std::pair<T, std::string>> options);
    Item action(std::string label, std::function<void()> fn);
    Item submenu(std::string label, std::function<Page()> build);
    Item back_item();
    Item binding(int button, bool keyboard);

    Page build_root();
    Page build_states();
    Page build_slot(int slot);
    Page build_video();
    Page build_audio();
    Page build_controls();
    Page build_bindings(bool keyboard);
    Page build_quit();
    Page build_gallery();

    Config& config_;
    MenuHost host_;
    std::vector<Page> stack_;
    bool open_ = false, changed_ = false, message_error_ = false;
    std::string message_;
    Capture capture_;
    std::array<std::string, kSlotCount> slot_text_;
};

// Escurece o fundo e desenha o menu em 'overlay' (320x224 RGBA; o resto fica transparente).
void draw_menu(const Menu& menu, video::Frame& overlay);
// Aviso curto na parte de baixo (F5/F7 etc.), desenhado quando o menu está fechado.
void draw_toast(video::Frame& overlay, const std::string& text, bool error);
// Compõe o overlay sobre o quadro do jogo (para capturas de diagnóstico).
void composite(video::Frame& base, const video::Frame& overlay);

}  // namespace frontend
