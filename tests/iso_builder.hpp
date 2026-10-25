#pragma once
#include <map>
#include <set>
#include <string>

#include "support.hpp"

namespace testsupport {

using Sector = std::vector<uint8_t>;

inline void put_both32(Sector& s, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[off + i] = (v >> (8 * i)) & 0xFF;
    for (int i = 0; i < 4; ++i) s[off + 4 + i] = (v >> (8 * (3 - i))) & 0xFF;
}

inline void put_record(Sector& dir, size_t& pos, const std::string& name, uint32_t lba, uint32_t size, bool is_dir) {
    size_t len = 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0);
    dir[pos] = static_cast<uint8_t>(len);
    put_both32(dir, pos + 2, lba);
    put_both32(dir, pos + 10, size);
    dir[pos + 25] = is_dir ? 2 : 0;
    dir[pos + 32] = static_cast<uint8_t>(name.size());
    std::memcpy(dir.data() + pos + 33, name.data(), name.size());
    pos += len;
}

// Disco sintético: setor 0 = cabeçalho Saturn, 16 = descritor primário ISO9660, 17+ = diretórios e dados.
// `files`: caminho ("A.TXT", "DIR/B.BIN") -> conteúdo. Nomes são gravados literalmente (com ";1" nos arquivos).
class SyntheticIso {
public:
    SyntheticIso(std::string product, std::string version) : product_(std::move(product)), version_(std::move(version)) {}

    SyntheticIso& add(const std::string& path, std::string content) {
        files_[path] = std::move(content);
        return *this;
    }

    std::vector<Sector> sectors() const {
        std::vector<Sector> out(17, Sector(2048, 0));
        auto field = [&](size_t off, const std::string& text, size_t width) {
            std::string padded = text + std::string(width - text.size(), ' ');
            std::memcpy(out[0].data() + off, padded.data(), width);
        };
        field(0x00, "SEGA SEGASATURN ", 16);
        field(0x10, "SEGA TP T-95", 16);
        field(0x20, product_, 10);
        field(0x2A, version_, 6);
        field(0x30, "19980427", 8);
        field(0x60, "TESTE", 112);

        std::set<std::string> dirs;
        for (const auto& [path, _] : files_)
            if (auto slash = path.find('/'); slash != std::string::npos) dirs.insert(path.substr(0, slash));

        // Layout: 17 = raiz, 18.. = um setor por subdiretório, depois os dados.
        std::map<std::string, uint32_t> dir_lba;
        uint32_t next = 18;
        for (const auto& d : dirs) dir_lba[d] = next++;
        std::map<std::string, uint32_t> file_lba;
        for (const auto& [path, content] : files_) {
            file_lba[path] = next;
            next += std::max<size_t>(1, (content.size() + 2047) / 2048);
        }

        Sector root(2048, 0);
        size_t pos = 0;
        put_record(root, pos, std::string(1, '\0'), 17, 2048, true);
        put_record(root, pos, std::string(1, '\1'), 17, 2048, true);
        for (const auto& d : dirs) put_record(root, pos, d, dir_lba[d], 2048, true);
        for (const auto& [path, content] : files_)
            if (path.find('/') == std::string::npos) put_record(root, pos, path + ";1", file_lba[path], content.size(), false);
        out.push_back(root);

        for (const auto& d : dirs) {
            Sector sub(2048, 0);
            size_t p = 0;
            put_record(sub, p, std::string(1, '\0'), dir_lba[d], 2048, true);
            put_record(sub, p, std::string(1, '\1'), 17, 2048, true);
            for (const auto& [path, content] : files_)
                if (path.rfind(d + "/", 0) == 0) put_record(sub, p, path.substr(d.size() + 1) + ";1", file_lba[path], content.size(), false);
            out.push_back(sub);
        }
        for (const auto& [path, content] : files_) {
            size_t n = std::max<size_t>(1, (content.size() + 2047) / 2048);
            for (size_t i = 0; i < n; ++i) {
                Sector s(2048, 0);
                size_t chunk = content.size() > i * 2048 ? std::min<size_t>(2048, content.size() - i * 2048) : 0;
                std::memcpy(s.data(), content.data() + i * 2048, chunk);
                out.push_back(s);
            }
        }

        Sector pvd(2048, 0);
        pvd[0] = 1;
        std::memcpy(pvd.data() + 1, "CD001", 5);
        pvd[6] = 1;
        size_t rp = 156;
        put_record(pvd, rp, std::string(1, '\0'), 17, 2048, true);
        out[16] = pvd;
        return out;
    }

    // Grava .bin + .cue em `dir` e devolve o caminho do .cue.
    std::filesystem::path write(const std::filesystem::path& dir) const {
        write_bin(dir / "d.bin", sectors());
        write_text(dir / "d.cue", "FILE \"d.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
        return dir / "d.cue";
    }

private:
    std::string product_, version_;
    std::map<std::string, std::string> files_;
};

}  // namespace testsupport
