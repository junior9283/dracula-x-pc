#include "saturnfs/cue.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace saturnfs {
namespace {

constexpr uintmax_t kRawSector = 2352;

std::string unquote(const std::string& rest) {
    auto first = rest.find('"');
    if (first == std::string::npos) return rest.substr(0, rest.find(' '));
    return rest.substr(first + 1, rest.rfind('"') - first - 1);
}

uint32_t parse_msf(const std::string& msf) {
    unsigned m = 0, s = 0, f = 0;
    if (std::sscanf(msf.c_str(), "%u:%u:%u", &m, &s, &f) != 3) throw std::runtime_error("INDEX inválido: " + msf);
    return (m * 60 + s) * 75 + f;
}

}  // namespace

std::vector<Track> parse_cue(const std::filesystem::path& cue) {
    std::ifstream in(cue);
    if (!in) throw std::runtime_error("não foi possível abrir " + cue.string());

    std::vector<Track> tracks;
    std::filesystem::path file;
    uint32_t file_base_lba = 0;
    uint32_t next_file_base = 0;

    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string keyword;
        words >> keyword;
        std::string rest;
        std::getline(words >> std::ws, rest);
        if (!rest.empty() && rest.back() == '\r') rest.pop_back();

        if (keyword == "FILE") {
            file = cue.parent_path() / unquote(rest);
            if (!std::filesystem::exists(file)) throw std::runtime_error("arquivo da faixa não encontrado: " + file.filename().string());
            file_base_lba = next_file_base;
            next_file_base += static_cast<uint32_t>(std::filesystem::file_size(file) / kRawSector);
        } else if (keyword == "TRACK") {
            std::istringstream fields(rest);
            int number = 0;
            std::string type;
            fields >> number >> type;
            tracks.push_back({number, type == "AUDIO" ? Track::Kind::Audio : Track::Kind::Data, file, file_base_lba});
        } else if (keyword == "INDEX" && !tracks.empty()) {
            std::istringstream fields(rest);
            int index = 0;
            std::string msf;
            fields >> index >> msf;
            if (index == 1) tracks.back().start_lba = file_base_lba + parse_msf(msf);
        }
    }
    return tracks;
}

}  // namespace saturnfs
