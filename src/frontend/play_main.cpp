// dracula-x: o jogo em tempo real (janela, som e controle via SDL3), rodando o núcleo do Saturn sobre o disco do próprio usuário.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <tuple>
#include <stdexcept>
#include <string>
#include <vector>

#include "frontend/config.hpp"
#include "frontend/font.hpp"
#include "frontend/gallery.hpp"
#include "frontend/input.hpp"
#include "frontend/machine_video.hpp"
#include "frontend/menu.hpp"
#include "frontend/states.hpp"
#include "audio/scsp.hpp"
#include "saturn/machine.hpp"
#include "saturnfs/image.hpp"
#include "saturnfs/package.hpp"
#include "saturnfs/verify.hpp"
#include "video/frame.hpp"

namespace {

using frontend::Config;

struct Options {
    std::string cue, install, boot_font, save_file, data_dir;
    int scale = 0;      // 0 = o da configuração
    long frames = -1;   // > 0: fecha depois de N quadros (teste de fumaça)
    bool fullscreen = false, audio = true;
    // Diagnóstico: roda alguns quadros, abre o menu (opcionalmente numa página, "Controles/Teclado"), grava o PNG e sai.
    std::string menu_shot, menu_path;
    int menu_select = -1;
    double menu_time = 4.0;   // galeria: segundos de animação/música simulados antes da captura
};

constexpr int kAudioRate = int(audio::Scsp::kSampleHz);   // a saída do SCSP vai direto ao dispositivo
constexpr int kBytesPerAudioFrame = 2 * sizeof(int16_t);   // estéreo S16
constexpr long kSaveCheckFrames = 300;   // ~5 s

constexpr const char* kPackageName = "dracula-x.pak";
constexpr const char* kSaveName = "dracula-x.bup";
constexpr const char* kConfigName = "dracula-x.ini";
// SHA-256 dos setores de usuário da faixa de dados do T-9527G V1.400 (só o resumo; nenhum dado do jogo).
constexpr const char* kSupportedDigest = "1902b428c12b7cc2b095a05667d6f546f742340eff7ef726fa348f833d972c43";

void usage() {
    std::cerr << "uso: dracula-x [disco.cue] [--install disco.cue] [--data-dir pasta] [--boot-font arquivo] [--save arquivo]\n"
                 "                [--scale N] [--fullscreen] [--no-audio] [--frames N]\n"
                 "sem disco, roda do pacote gerado por --install (o disco não é mais necessário depois disso)\n"
                 "teclado (padrão): setas, Enter = Start, Z/X/C = A/B/C, A/S/D = X/Y/Z, Q/W = L/R\n"
                 "Esc ou Back/Guide do controle: menu (save states, vídeo, áudio, controles); F5/F7 salva/carrega, F6/F8 trocam o slot;\n"
                 "F11 tela cheia, F12 captura\n";
}

std::optional<Options> parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("faltou o valor de " + arg);
            return argv[++i];
        };
        if (arg == "--boot-font") o.boot_font = value();
        else if (arg == "--save") o.save_file = value();
        else if (arg == "--install") o.install = value();
        else if (arg == "--data-dir") o.data_dir = value();
        else if (arg == "--frames") o.frames = std::stol(value());
        else if (arg == "--scale") o.scale = std::clamp(std::stoi(value()), 1, 8);
        else if (arg == "--menu-shot") o.menu_shot = value();
        else if (arg == "--menu-path") o.menu_path = value();
        else if (arg == "--menu-select") o.menu_select = std::stoi(value());
        else if (arg == "--menu-time") o.menu_time = std::stod(value());
        else if (arg == "--fullscreen") o.fullscreen = true;
        else if (arg == "--no-audio") o.audio = false;
        else if (arg == "--help" || arg == "-h") return std::nullopt;
        else if (!arg.empty() && arg[0] != '-' && o.cue.empty()) o.cue = arg;
        else throw std::invalid_argument("argumento desconhecido: " + arg);
    }
    return o;
}

// Memória de backup (saves), persistida num arquivo na pasta de dados.
void load_backup(saturn::Machine& machine, const std::string& file) {
    const auto data = frontend::try_read_file_bytes(file);
    if (!data) return;
    if (data->size() != saturn::Machine::kBackupRamSize) {
        std::cerr << "save ignorado (tamanho inesperado): " << file << '\n';
        return;
    }
    machine.load_backup_ram(*data);
}

// Grava a memória de backup quando ela muda (verificado a cada poucos segundos e na saída): um travamento ou queda de energia
// perde no máximo esses segundos, não a sessão inteira.
class BackupSaver {
public:
    BackupSaver(saturn::Machine& machine, std::string file) : machine_(machine), file_(std::move(file)) {
        const auto ram = machine_.backup_ram();
        saved_.assign(ram.begin(), ram.end());
    }

    void save_if_changed() {
        const auto ram = machine_.backup_ram();
        if (std::equal(ram.begin(), ram.end(), saved_.begin())) return;
        try {
            frontend::write_file_atomic(file_, ram);   // troca atômica: um save antigo nunca fica pela metade
        } catch (const std::exception& e) {
            std::cerr << "save do jogo: " << e.what() << '\n';
            return;
        }
        saved_.assign(ram.begin(), ram.end());
    }

private:
    saturn::Machine& machine_;
    std::string file_;
    std::vector<uint8_t> saved_;
};

class Window {
public:
    Window(const Config& config, int scale) {
        scale = scale > 0 ? scale : config.window_scale;
        if (!SDL_CreateWindowAndRenderer("Akumajou Dracula X", 320 * scale, 224 * scale, SDL_WINDOW_RESIZABLE, &window_, &renderer_))
            throw std::runtime_error(std::string("SDL: ") + SDL_GetError());
        overlay_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, frontend::kOverlayW, frontend::kOverlayH);
        SDL_SetTextureBlendMode(overlay_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(overlay_, SDL_SCALEMODE_NEAREST);
        apply(config);
    }
    ~Window() {
        for (SDL_Texture* t : {texture_, scanlines_, overlay_})
            if (t) SDL_DestroyTexture(t);
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
    }

    // Opções de vídeo que o SDL guarda no estado da janela/renderizador.
    void apply(const Config& config) {
        if (bool(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != config.fullscreen) SDL_SetWindowFullscreen(window_, config.fullscreen);
        SDL_SetRenderVSync(renderer_, config.vsync ? 1 : 0);   // desligado: o ritmo é dado pelo áudio (ou pelo relógio)
    }

    // Desenha o quadro do jogo, as scanlines (se ligadas) e o overlay (menu/avisos) por cima, na área calculada pela configuração.
    void present(const video::Frame& frame, const Config& config, const video::Frame* overlay) {
        if (!texture_ || frame.width != width_ || frame.height != height_) {
            for (SDL_Texture* t : {texture_, scanlines_})
                if (t) SDL_DestroyTexture(t);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
            width_ = frame.width;
            height_ = frame.height;
            // Padrão de scanlines: 1 x (2 * altura); a linha de baixo de cada par escurece.
            std::vector<uint32_t> lines(size_t(2) * height_, 0);
            for (size_t i = 1; i < lines.size(); i += 2) lines[i] = video::rgba(0, 0, 0, 110);
            scanlines_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 2 * height_);
            SDL_UpdateTexture(scanlines_, nullptr, lines.data(), 4);
            SDL_SetTextureBlendMode(scanlines_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(scanlines_, SDL_SCALEMODE_NEAREST);
        }
        SDL_SetTextureScaleMode(texture_, config.filter == frontend::Filter::kSmooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_UpdateTexture(texture_, nullptr, frame.pixels.data(), frame.width * 4);

        int win_w = 0, win_h = 0;
        SDL_GetCurrentRenderOutputSize(renderer_, &win_w, &win_h);
        area_ = frontend::game_rect(config, win_w, win_h, frame.width, frame.height);
        const SDL_FRect dst{float(area_.x), float(area_.y), float(area_.w), float(area_.h)};
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
        if (config.scanlines) SDL_RenderTexture(renderer_, scanlines_, nullptr, &dst);
        if (overlay) {
            SDL_UpdateTexture(overlay_, nullptr, overlay->pixels.data(), overlay->width * 4);
            SDL_RenderTexture(renderer_, overlay_, nullptr, &dst);
        }
        SDL_RenderPresent(renderer_);
    }

    // Coordenadas de mouse em pixels do overlay de 320x224 (fora da imagem dão valores fora da faixa).
    void convert(SDL_Event& e) { SDL_ConvertEventToRenderCoordinates(renderer_, &e); }
    std::pair<int, int> to_overlay(float x, float y) const {
        if (area_.w <= 0 || area_.h <= 0) return {-1, -1};
        return {int(std::floor((x - area_.x) * frontend::kOverlayW / area_.w)), int(std::floor((y - area_.y) * frontend::kOverlayH / area_.h))};
    }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr, *scanlines_ = nullptr, *overlay_ = nullptr;
    int width_ = 0, height_ = 0;
    frontend::Rect area_;
};

// Confere que a fonte é o T-9527G V1.400 completo (vale para o disco e para o pacote).
void require_supported(const saturnfs::SectorReader& source) {
    saturnfs::require_supported_disc(saturnfs::read_header(source));
    saturnfs::require_complete_image(source);
}

std::unique_ptr<saturnfs::SectorReader> open_disc(const std::string& cue) {
    auto disc = std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(cue));
    require_supported(*disc);
    return disc;
}

// Pasta de dados: --data-dir, ou a pasta do executável se já houver um pacote lá (modo portátil), ou a pasta do usuário.
std::filesystem::path data_dir(const Options& o) {
    if (!o.data_dir.empty()) return o.data_dir;
    if (const char* base = SDL_GetBasePath(); base && std::filesystem::exists(std::filesystem::path(base) / kPackageName)) return base;
    char* pref = SDL_GetPrefPath("dracula-x-pc", "dracula-x");
    if (!pref) throw std::runtime_error(std::string("pasta de dados do usuário indisponível: ") + SDL_GetError());
    std::filesystem::path dir = pref;
    SDL_free(pref);
    return dir;
}

void install(const std::string& cue, const std::filesystem::path& package) {
    const auto disc = open_disc(cue);
    std::filesystem::create_directories(package.parent_path());
    std::cout << "gerando o pacote em " << package.string() << " ...\n";
    uint32_t shown = ~0u;
    saturnfs::write_package(*disc, package, [&shown](uint32_t done, uint32_t total) {
        if (const uint32_t percent = done * 100 / total; percent != shown) std::cout << "\r  " << (shown = percent) << "%" << std::flush;
    }, kSupportedDigest);
    std::cout << "\npacote pronto; o disco não é mais necessário.\n";
}

// Sem pacote: pede o .cue numa janela de diálogo do sistema. Devolve vazio se o usuário cancelar.
std::string ask_for_cue() {
    struct Result { bool done = false; std::string path; } result;
    static constexpr SDL_DialogFileFilter kFilters[] = {{"Imagem do disco (cue)", "cue"}};
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Akumajou Dracula X",
                             "Primeira execução: escolha o arquivo .cue do seu disco (T-9527G V1.400). "
                             "O jogo gera um pacote local e depois não precisa mais do disco.", nullptr);
    SDL_ShowOpenFileDialog([](void* user, const char* const* files, int) {
        auto& r = *static_cast<Result*>(user);
        if (files && files[0]) r.path = files[0];
        r.done = true;
    }, &result, nullptr, kFilters, 1, nullptr, false);
    while (!result.done) {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 50);
    }
    return result.path;
}

// Nomes de tecla em português para o menu (o SDL só traz os nomes em inglês).
std::string key_name(int scancode) {
    static const std::pair<const char*, const char*> kTranslations[] = {
        {"Return", "Enter"}, {"Left", "Seta esquerda"}, {"Right", "Seta direita"}, {"Up", "Seta cima"}, {"Down", "Seta baixo"},
        {"Space", "Espaço"}, {"Left Shift", "Shift esq."}, {"Right Shift", "Shift dir."}, {"Left Ctrl", "Ctrl esq."},
        {"Right Ctrl", "Ctrl dir."}, {"Left Alt", "Alt esq."}, {"Right Alt", "Alt dir."}, {"Backspace", "Backspace"}, {"Tab", "Tab"},
    };
    const std::string name = SDL_GetScancodeName(SDL_Scancode(scancode));
    if (name.empty()) return "Tecla " + std::to_string(scancode);
    for (const auto& [english, portuguese] : kTranslations)
        if (name == english) return portuguese;
    return name;
}

// Controle aberto (se houver) e consulta de botões/analógico por código de entrada.
struct Pads {
    SDL_Gamepad* gamepad = nullptr;

    bool button(int code) const {
        if (!gamepad) return false;
        constexpr int16_t kTrigger = 12000;
        if (code == frontend::kPadLeftTrigger) return SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTrigger;
        if (code == frontend::kPadRightTrigger) return SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTrigger;
        return SDL_GetGamepadButton(gamepad, SDL_GamepadButton(code));
    }
    uint16_t stick(int deadzone_percent) const {
        if (!gamepad) return 0;
        return frontend::stick_bits(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY),
                                    deadzone_percent);
    }
};

// Os valores dos botões do SDL usados nos padrões e na captura precisam bater com os de frontend/input.cpp.
static_assert(SDL_SCANCODE_ESCAPE == frontend::kScancodeEscape && SDL_SCANCODE_UP == 82 && SDL_SCANCODE_RETURN == 40);
static_assert(SDL_GAMEPAD_BUTTON_DPAD_UP == 11 && SDL_GAMEPAD_BUTTON_START == 6 && SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER == 10);

// Entrega a leitura de setores a quem só enxerga uma fonte emprestada: a máquina é dona do disco, a galeria lê o mesmo.
class SharedSectorReader : public saturnfs::SectorReader {
public:
    explicit SharedSectorReader(std::shared_ptr<const saturnfs::SectorReader> source) : source_(std::move(source)) {}
    std::array<uint8_t, saturnfs::kUserSectorSize> read_user_sector(uint32_t lba) const override { return source_->read_user_sector(lba); }
    uint32_t sector_count() const override { return source_->sector_count(); }

private:
    std::shared_ptr<const saturnfs::SectorReader> source_;
};

// Modo de diagnóstico (--menu-shot): grava o menu composto sobre o último quadro do jogo, sem abrir janela.
void write_menu_shot(saturn::Machine& machine, const Options& o, frontend::Config config, const std::filesystem::path& dir,
                     frontend::Gallery& gallery) {
    for (long i = 0; i < (o.frames > 0 ? o.frames : 120); ++i) machine.run_frame();
    frontend::MenuHost host;
    host.key_name = key_name;
    host.slot_text = [&dir](int slot) { return frontend::slot_description(frontend::state_path(dir, slot)); };
    host.gallery = &gallery;
    frontend::Menu menu(config, host);
    std::vector<std::string> path;
    std::stringstream parts(o.menu_path);
    for (std::string part; std::getline(parts, part, '/');) path.push_back(part);
    if (!menu.open_path(path)) throw std::runtime_error("página do menu não encontrada: " + o.menu_path);
    if (o.menu_select >= 0) menu.select(o.menu_select);
    if (menu.in_gallery()) {   // toca a faixa selecionada "no escuro": um dispositivo de áudio simulado drena a fila em tempo real
        gallery.play(gallery.selected());
        double queued_frames = 0;
        constexpr double kStep = 1.0 / 60;
        for (double t = 0; t < o.menu_time; t += kStep) {
            gallery.pump(int(queued_frames) * 4, [&queued_frames](std::span<const int16_t> pcm) { queued_frames += double(pcm.size() / 2); });
            queued_frames = std::max(0.0, queued_frames - frontend::Gallery::kSampleRate * kStep);
            gallery.advance(kStep);
        }
    }
    video::Frame shot = frontend::capture_frame(machine);
    video::Frame overlay(frontend::kOverlayW, frontend::kOverlayH);
    frontend::draw_menu(menu, overlay);
    frontend::composite(shot, overlay);
    video::write_png(o.menu_shot, shot);
    std::cout << "menu gravado em " << o.menu_shot << '\n';
}

int run(const Options& o) {
    const auto dir = data_dir(o);
    const auto package = dir / kPackageName;
    if (!o.install.empty()) {
        install(o.install, package);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (o.audio ? SDL_INIT_AUDIO : 0)))
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());

    std::unique_ptr<saturnfs::SectorReader> disc;
    if (!o.cue.empty()) {
        disc = open_disc(o.cue);
    } else {
        if (!std::filesystem::exists(package)) {
            const auto cue = ask_for_cue();
            if (cue.empty()) { SDL_Quit(); return 0; }
            install(cue, package);
        }
        auto reader = std::make_unique<saturnfs::PackageReader>(saturnfs::PackageReader::open(package));
        require_supported(*reader);
        if (reader->digest() != kSupportedDigest)
            throw std::runtime_error("pacote de outra versão do disco: gere de novo com dracula-x --install <disco.cue>");
        disc = std::move(reader);
    }
    const std::string save_file = o.save_file.empty() ? (dir / kSaveName).string() : o.save_file;
    const auto config_file = dir / kConfigName;
    frontend::Config config = frontend::load_config(config_file);
    if (o.fullscreen) config.fullscreen = true;
    frontend::Config saved_config = config;   // para só regravar o arquivo quando algo mudou

    const std::shared_ptr<saturnfs::SectorReader> shared_disc = std::move(disc);
    frontend::Gallery gallery(*shared_disc);
    saturn::Machine machine(std::make_unique<SharedSectorReader>(shared_disc));
    machine.set_bios_strict(false);
    machine.set_boot_font_required(false);
    if (!o.boot_font.empty()) {
        const auto font = frontend::try_read_file_bytes(o.boot_font);
        if (!font) throw std::runtime_error("não foi possível abrir a fonte do boot: " + o.boot_font);
        machine.set_boot_font(*font);
    }
    machine.boot();
    load_backup(machine, save_file);
    if (!o.menu_shot.empty()) {
        write_menu_shot(machine, o, config, dir, gallery);
        SDL_Quit();
        return 0;
    }
    Window window(config, o.scale);
    SDL_HideCursor();

    SDL_AudioStream* audio = nullptr;
    if (o.audio) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kAudioRate};
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio) {
            SDL_ResumeAudioStreamDevice(audio);
            machine.scsp().set_audio_enabled(true);
        } else {
            std::cerr << "sem áudio: " << SDL_GetError() << '\n';
        }
    }
    std::vector<int16_t> samples(kAudioRate / 10 * 2);
    BackupSaver backup(machine, save_file);

    // Save states: <pasta de dados>/states/slotN.dxs (a máquina é que sabe serializar o estado).
    const auto save_slot = [&](int slot) { frontend::write_file_atomic(frontend::state_path(dir, slot), machine.save_state()); };
    video::Frame frame = frontend::capture_frame(machine);
    const auto load_slot = [&](int slot) {
        const auto file = frontend::state_path(dir, slot);
        if (!std::filesystem::exists(file)) throw std::runtime_error("o slot " + std::to_string(slot + 1) + " está vazio");
        machine.load_state(frontend::read_file_bytes(file));
        frame = frontend::capture_frame(machine);
        machine.scsp().drain_audio(samples);   // descarta o som do estado anterior
        if (audio) SDL_ClearAudioStream(audio);
    };

    frontend::MenuHost host;
    host.key_name = key_name;
    host.slot_text = [&dir](int slot) { return frontend::slot_description(frontend::state_path(dir, slot)); };
    host.save_state = save_slot;
    host.load_state = load_slot;
    host.gallery = &gallery;
    bool running = true;
    host.quit = [&running] { running = false; };
    frontend::Menu menu(config, host);

    struct Toast { std::string text; bool error = false; uint64_t until_ns = 0; } toast;
    const auto show_toast = [&toast](std::string text, bool error = false) {
        toast = {std::move(text), error, SDL_GetTicksNS() + 2'000'000'000ull};
    };
    const auto apply_options = [&] {
        window.apply(config);
        if (audio) SDL_SetAudioStreamGain(audio, config.mute ? 0.0f : float(config.volume) / 100.0f);
    };
    const auto hotkey_state = [&](bool save) {
        try {
            if (save) save_slot(config.slot);
            else load_slot(config.slot);
            show_toast(std::string(save ? "Estado salvo no slot " : "Estado carregado do slot ") + std::to_string(config.slot + 1));
        } catch (const std::exception& e) {
            show_toast(std::string("Erro: ") + e.what(), true);
        }
    };
    const auto change_slot = [&](int step) {
        config.slot = (config.slot + step + frontend::kSlotCount) % frontend::kSlotCount;
        show_toast("Slot " + std::to_string(config.slot + 1) + ": " + frontend::slot_description(frontend::state_path(dir, config.slot)));
    };
    const auto save_config_if_changed = [&] {
        if (config == saved_config) return;
        if (frontend::save_config(config_file, config)) saved_config = config;
        else std::cerr << "não foi possível gravar " << config_file.string() << '\n';
    };
    apply_options();

    Pads pads;
    std::array<frontend::Repeater, 4> nav_repeat;   // cima, baixo, esquerda, direita do controle dentro do menu
    int mouse_x = -1, mouse_y = -1;                 // posição do mouse no overlay
    bool menu_was_open = false, gallery_audio = false;   // gallery_audio: o dispositivo está tocando a galeria
    uint64_t next_frame = SDL_GetTicksNS();
    int shots = 0;
    long frames_left = o.frames;
    video::Frame overlay(frontend::kOverlayW, frontend::kOverlayH);
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!pads.gamepad) pads.gamepad = SDL_OpenGamepad(e.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (pads.gamepad && SDL_GetGamepadID(pads.gamepad) == e.gdevice.which) {
                        SDL_CloseGamepad(pads.gamepad);
                        pads.gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    const SDL_Scancode key = e.key.scancode;
                    if (key == SDL_SCANCODE_F11 && !e.key.repeat) {
                        config.fullscreen = !config.fullscreen;
                        window.apply(config);
                    } else if (menu.is_open()) {
                        using frontend::Nav;
                        if (menu.capturing()) {
                            if (!e.key.repeat) menu.capture_key(key);
                            break;
                        }
                        switch (key) {
                            case SDL_SCANCODE_ESCAPE: if (!e.key.repeat) menu.handle(Nav::kBack); break;
                            case SDL_SCANCODE_BACKSPACE: menu.handle(Nav::kBack); break;
                            case SDL_SCANCODE_UP: menu.handle(Nav::kUp); break;
                            case SDL_SCANCODE_DOWN: menu.handle(Nav::kDown); break;
                            case SDL_SCANCODE_LEFT: menu.handle(Nav::kLeft); break;
                            case SDL_SCANCODE_RIGHT: menu.handle(Nav::kRight); break;
                            case SDL_SCANCODE_PAGEUP: menu.handle(Nav::kPageUp); break;
                            case SDL_SCANCODE_PAGEDOWN: menu.handle(Nav::kPageDown); break;
                            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
                                if (!e.key.repeat) menu.handle(Nav::kConfirm);
                                break;
                            default: break;
                        }
                    } else if (!e.key.repeat) {
                        if (key == SDL_SCANCODE_ESCAPE) menu.open();
                        else if (key == SDL_SCANCODE_F5) hotkey_state(true);
                        else if (key == SDL_SCANCODE_F7) hotkey_state(false);
                        else if (key == SDL_SCANCODE_F6) change_slot(-1);
                        else if (key == SDL_SCANCODE_F8) change_slot(+1);
                        else if (key == SDL_SCANCODE_F12) {
                            // Data/hora no nome: capturas de sessões anteriores não são sobrescritas.
                            const std::time_t now = std::time(nullptr);
                            char stamp[32] = "sem-data";
                            if (const std::tm* local = std::localtime(&now)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", local);
                            const std::string file = (dir / ("captura-" + std::string(stamp) + "-" + std::to_string(++shots) + ".png")).string();
                            video::write_png(file, frame);
                            std::cout << "captura gravada em " << file << '\n';
                        }
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                    using frontend::Nav;
                    const int button = e.gbutton.button;
                    const bool opener = button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_GUIDE;
                    if (!menu.is_open()) {
                        if (opener) menu.open();
                    } else if (menu.capturing()) {
                        menu.capture_pad(button);
                    } else if (opener) {
                        menu.close();
                    } else if (button == SDL_GAMEPAD_BUTTON_SOUTH) {
                        menu.handle(Nav::kConfirm);
                    } else if (button == SDL_GAMEPAD_BUTTON_EAST) {
                        menu.handle(Nav::kBack);
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:   // gatilho durante a captura
                    if (menu.capturing() && e.gaxis.value > 20000) {
                        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) menu.capture_pad(frontend::kPadLeftTrigger);
                        else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) menu.capture_pad(frontend::kPadRightTrigger);
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (menu.is_open()) {
                        window.convert(e);
                        std::tie(mouse_x, mouse_y) = window.to_overlay(e.motion.x, e.motion.y);
                        menu.hover(menu.row_at(mouse_x, mouse_y));
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (menu.is_open()) {
                        window.convert(e);
                        std::tie(mouse_x, mouse_y) = window.to_overlay(e.button.x, e.button.y);
                        if (e.button.button == SDL_BUTTON_LEFT) menu.click(menu.row_at(mouse_x, mouse_y));
                        else if (e.button.button == SDL_BUTTON_RIGHT) menu.handle(frontend::Nav::kBack);
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    if (menu.is_open() && e.wheel.y != 0) menu.wheel(e.wheel.y > 0 ? 1 : -1, menu.row_at(mouse_x, mouse_y));
                    break;
                default: break;
            }
        }

        if (menu.take_changed()) apply_options();
        if (menu.is_open() != menu_was_open) {   // abriu ou fechou: pausa/retoma o som e guarda a configuração
            menu_was_open = menu.is_open();
            if (menu_was_open) {
                SDL_ShowCursor();
                if (audio) SDL_PauseAudioStreamDevice(audio);
            } else {
                SDL_HideCursor();
                gallery_audio = false;
                save_config_if_changed();
                if (audio) {
                    SDL_ClearAudioStream(audio);
                    SDL_ResumeAudioStreamDevice(audio);
                }
                if (!menu.message().empty()) show_toast(menu.message(), menu.message_is_error());
                next_frame = SDL_GetTicksNS();
            }
        }

        const uint64_t now_ns = SDL_GetTicksNS();
        const auto clear_overlay = [&overlay] { std::fill(overlay.pixels.begin(), overlay.pixels.end(), 0u); };
        if (menu.is_open()) {
            // Menu aberto: a emulação e o som ficam parados; o controle navega (direcional/analógico com repetição).
            if (!menu.capturing()) {
                const uint16_t stick = pads.stick(std::max(config.deadzone, 40));
                const bool down[4] = {pads.button(SDL_GAMEPAD_BUTTON_DPAD_UP) || (stick & frontend::pad_bit(frontend::kUp)),
                                      pads.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || (stick & frontend::pad_bit(frontend::kDown)),
                                      pads.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || (stick & frontend::pad_bit(frontend::kLeft)),
                                      pads.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (stick & frontend::pad_bit(frontend::kRight))};
                constexpr frontend::Nav kNav[4] = {frontend::Nav::kUp, frontend::Nav::kDown, frontend::Nav::kLeft, frontend::Nav::kRight};
                for (int i = 0; i < 4; ++i)
                    if (nav_repeat[i].update(down[i], now_ns / 1'000'000)) menu.handle(kNav[i]);
            }
            if (audio) {   // galeria: o som do jogo cala e a música decodificada em streaming ocupa o dispositivo
                if (gallery.take_flush()) SDL_ClearAudioStream(audio);
                if (gallery.playing() != gallery_audio) {
                    gallery_audio = gallery.playing();
                    if (gallery_audio) SDL_ResumeAudioStreamDevice(audio);
                    else SDL_PauseAudioStreamDevice(audio);
                }
                if (gallery_audio)
                    gallery.pump(SDL_GetAudioStreamQueued(audio), [audio](std::span<const int16_t> pcm) {
                        SDL_PutAudioStreamData(audio, pcm.data(), int(pcm.size() * sizeof(int16_t)));
                    });
            }
            gallery.advance(1.0 / 60);
            clear_overlay();
            frontend::draw_menu(menu, overlay);
            window.present(frame, config, &overlay);
            SDL_DelayPrecise(16'000'000);
            continue;
        }

        const int deadzone = config.deadzone;
        const uint16_t pressed = frontend::map_inputs(
            config.bindings,
            [keys = SDL_GetKeyboardState(nullptr)](int code) { return keys[code]; },
            [&pads](int code) { return pads.button(code); }) | pads.stick(deadzone);
        machine.smpc().set_pad(static_cast<uint16_t>(~pressed));
        machine.run_frame();
        frame = frontend::capture_frame(machine);
        const bool toast_visible = now_ns < toast.until_ns;
        if (toast_visible) {   // o overlay só é limpo e enviado quando há algo nele
            clear_overlay();
            frontend::draw_toast(overlay, toast.text, toast.error);
        }
        window.present(frame, config, toast_visible ? &overlay : nullptr);
        if (frames_left > 0 && --frames_left == 0) running = false;
        if (machine.frame() % kSaveCheckFrames == 0) backup.save_if_changed();

        // Latência alvo do áudio (config): N quadros na fila. O relógio dá o ritmo; a fila só corrige a deriva entre o relógio do
        // sistema e o do dispositivo de som (cheia demais: espera um pouco; quase vazia: não espera neste quadro).
        // Duração do quadro no modo de vídeo atual: o mesmo relógio que gera as amostras do SCSP, então vídeo e áudio andam juntos.
        const uint64_t frame_ns = machine.frame_ns();
        next_frame += frame_ns;
        if (audio) {
            const int target_queued = int(uint64_t(kAudioRate) * frontend::latency_frames(config.latency) * frame_ns / 1'000'000'000ull) * kBytesPerAudioFrame;
            if (const size_t n = machine.scsp().drain_audio(samples)) SDL_PutAudioStreamData(audio, samples.data(), int(n * sizeof(int16_t)));
            const int queued = SDL_GetAudioStreamQueued(audio);
            if (queued > 2 * target_queued) next_frame += frame_ns / 8;
            else if (queued < target_queued / 2) next_frame = SDL_GetTicksNS();
        }
        const uint64_t now = SDL_GetTicksNS();
        if (next_frame > now) SDL_DelayPrecise(next_frame - now);
        else next_frame = now;   // atrasado: não tenta recuperar quadros perdidos
    }

    backup.save_if_changed();
    save_config_if_changed();
    if (pads.gamepad) SDL_CloseGamepad(pads.gamepad);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        if (!options) { usage(); return 2; }
        return run(*options);
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Akumajou Dracula X", e.what(), nullptr);
        return 1;
    }
}
