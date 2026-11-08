#include "frontend/config.hpp"

#include "frontend/fileio.hpp"
#include "saturnfs/strutil.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace frontend {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::optional<bool> parse_bool(std::string_view v) {
    const std::string s = saturnfs::to_lower(v);
    if (s == "1" || s == "true" || s == "sim" || s == "on" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "nao" || s == "não" || s == "off" || s == "no") return false;
    return std::nullopt;
}

std::optional<int> parse_int(std::string_view v) {
    int value = 0;
    const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), value);
    if (error != std::errc{} || end != v.data() + v.size()) return std::nullopt;
    return value;
}

// Atribui só se o valor for válido; do contrário o campo fica como está (o padrão).
void set_int(int& field, std::string_view v, int lo, int hi) {
    if (const auto n = parse_int(v); n && *n >= lo && *n <= hi) field = *n;
}
void set_bool(bool& field, std::string_view v) {
    if (const auto b = parse_bool(v)) field = *b;
}
template <class T, size_t N>
void set_enum(T& field, std::string_view v, const std::pair<const char*, T> (&names)[N]) {
    const std::string s = saturnfs::to_lower(v);
    for (const auto& [name, value] : names)
        if (s == name) field = value;
}

constexpr std::pair<const char*, Aspect> kAspects[] = {{"original", Aspect::kOriginal}, {"4:3", Aspect::k4x3}, {"stretch", Aspect::kStretch}};
constexpr std::pair<const char*, Filter> kFilters[] = {{"sharp", Filter::kSharp}, {"smooth", Filter::kSmooth}};
constexpr std::pair<const char*, Latency> kLatencies[] = {{"low", Latency::kLow}, {"medium", Latency::kMedium}, {"high", Latency::kHigh}};
constexpr const char* kButtonKeys[kButtonCount] = {"up", "down", "left", "right", "start", "a", "b", "c", "x", "y", "z", "l", "r"};

template <class T, size_t N>
const char* enum_name(T value, const std::pair<const char*, T> (&names)[N]) {
    for (const auto& [name, v] : names)
        if (v == value) return name;
    return names[0].first;
}

}  // namespace

int latency_frames(Latency latency) { return latency == Latency::kLow ? 2 : latency == Latency::kMedium ? 3 : 6; }

Config parse_config(std::string_view text) {
    Config c;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        const std::string_view view = trim(std::string_view(line).substr(0, line.find('#')));   // comentário no fim da linha
        if (view.empty() || view.front() == '#' || view.front() == ';') continue;
        const size_t eq = view.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = saturnfs::to_lower(trim(view.substr(0, eq)));
        const std::string_view v = trim(view.substr(eq + 1));
        if (key == "fullscreen") set_bool(c.fullscreen, v);
        else if (key == "integer_scale") set_bool(c.integer_scale, v);
        else if (key == "aspect") set_enum(c.aspect, v, kAspects);
        else if (key == "filter") set_enum(c.filter, v, kFilters);
        else if (key == "scanlines") set_bool(c.scanlines, v);
        else if (key == "vsync") set_bool(c.vsync, v);
        else if (key == "window_scale") set_int(c.window_scale, v, 1, 8);
        else if (key == "volume") { set_int(c.volume, v, 0, 100); c.volume = c.volume / 5 * 5; }
        else if (key == "mute") set_bool(c.mute, v);
        else if (key == "latency") set_enum(c.latency, v, kLatencies);
        else if (key == "deadzone") set_int(c.deadzone, v, 5, 60);
        else if (key == "slot") { int slot = c.slot + 1; set_int(slot, v, 1, kSlotCount); c.slot = slot - 1; }
        else if (key.rfind("key.", 0) == 0 || key.rfind("pad.", 0) == 0) {
            const bool is_key = key[0] == 'k';
            const auto* name = std::find(std::begin(kButtonKeys), std::end(kButtonKeys), key.substr(4));
            const auto n = parse_int(v);
            if (name == std::end(kButtonKeys) || !n) continue;
            const int b = int(name - std::begin(kButtonKeys));
            if (is_key && valid_key_code(*n)) c.bindings.keys[b] = *n;
            if (!is_key && valid_pad_code(*n)) c.bindings.pad[b] = *n;
        }
    }
    return c;
}

std::string serialize_config(const Config& c) {
    std::ostringstream out;
    const auto flag = [](bool b) { return b ? "1" : "0"; };
    out << "# Configuração do Akumajou Dracula X (port para PC). Editável à mão; valores inválidos voltam ao padrão.\n"
        << "# Também pode ser mudada pelo menu do jogo (Esc).\n\n"
        << "[video]\n"
        << "fullscreen = " << flag(c.fullscreen) << "\n"
        << "integer_scale = " << flag(c.integer_scale) << "\n"
        << "aspect = " << enum_name(c.aspect, kAspects) << "   # original, 4:3, stretch\n"
        << "filter = " << enum_name(c.filter, kFilters) << "   # sharp, smooth\n"
        << "scanlines = " << flag(c.scanlines) << "\n"
        << "vsync = " << flag(c.vsync) << "\n"
        << "window_scale = " << c.window_scale << "   # 1 a 8\n\n"
        << "[audio]\n"
        << "volume = " << c.volume << "   # 0 a 100\n"
        << "mute = " << flag(c.mute) << "\n"
        << "latency = " << enum_name(c.latency, kLatencies) << "   # low, medium, high\n\n"
        << "[controls]\n"
        << "deadzone = " << c.deadzone << "   # % do curso do analógico, 5 a 60\n"
        << "# key.* = scancode do SDL; pad.* = botão do controle do SDL (1000/1001 = gatilhos); -1 = nenhum\n";
    for (int b = 0; b < kButtonCount; ++b) out << "key." << kButtonKeys[b] << " = " << c.bindings.keys[b] << "\n";
    for (int b = 0; b < kButtonCount; ++b) out << "pad." << kButtonKeys[b] << " = " << c.bindings.pad[b] << "\n";
    out << "\n[states]\nslot = " << c.slot + 1 << "   # 1 a " << kSlotCount << "\n";
    return out.str();
}

Config load_config(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parse_config(text.str());
}

bool save_config(const std::filesystem::path& file, const Config& config) {
    const std::string text = serialize_config(config);
    try {
        write_file_atomic(file, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Rect game_rect(const Config& c, int win_w, int win_h, int frame_w, int frame_h) {
    if (win_w <= 0 || win_h <= 0) return {};
    if (c.aspect == Aspect::kStretch) return {0, 0, win_w, win_h};
    // Tamanho "virtual" da imagem: 4:3 alarga o quadro para a proporção de um monitor de tubo.
    const double vw = c.aspect == Aspect::k4x3 ? frame_h * 4.0 / 3.0 : frame_w, vh = frame_h;
    double scale = std::min(win_w / vw, win_h / vh);
    if (c.integer_scale && scale >= 1.0) scale = std::floor(scale);
    const int w = int(std::lround(vw * scale)), h = int(std::lround(vh * scale));
    return {(win_w - w) / 2, (win_h - h) / 2, w, h};
}

}  // namespace frontend
