#pragma once
// Serialização simples para save states: bytes little-endian, seções com tag de 4 caracteres + tamanho.
// Regra: estado derivado (caches) ou só de saída/diagnóstico (anel de áudio, logs) não é serializado; o load o zera ou o mantém.
// O StateReader nunca lê além do fim: qualquer acesso fora do bloco, tag inesperada ou sobra numa seção lança StateError.
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace saturn {

struct StateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Checksum do conteúdo de um save state: uma palavra de 64 bits por vez (o conteúdo tem alguns MB).
inline uint64_t state_checksum(std::span<const uint8_t> data) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ data.size();
    size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        uint64_t w;
        std::memcpy(&w, data.data() + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < data.size(); ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

class StateWriter {
public:
    StateWriter() = default;
    explicit StateWriter(size_t reserve) { bytes_.reserve(reserve); }

    void u8(uint8_t v) { bytes_.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v) { put(v); }
    void u32(uint32_t v) { put(v); }
    void u64(uint64_t v) { put(v); }
    void i16(int16_t v) { put(static_cast<uint16_t>(v)); }
    void i32(int32_t v) { put(static_cast<uint32_t>(v)); }
    void i64(int64_t v) { put(static_cast<uint64_t>(v)); }
    void raw(std::span<const uint8_t> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }
    // Vetor de bytes com prefixo de tamanho.
    void blob(std::span<const uint8_t> data) { u64(data.size()); raw(data); }
    void text(const std::string& s) { blob({reinterpret_cast<const uint8_t*>(s.data()), s.size()}); }

    // Seção: tag de 4 caracteres, tamanho (u32) e o conteúdo escrito por `body`.
    template <class F> void section(const char (&tag)[5], F&& body) {
        raw({reinterpret_cast<const uint8_t*>(tag), 4});
        const size_t size_at = bytes_.size();
        u32(0);
        body();
        const uint32_t size = static_cast<uint32_t>(bytes_.size() - size_at - 4);
        for (int i = 0; i < 4; ++i) bytes_[size_at + i] = static_cast<uint8_t>(size >> (8 * i));
    }

    size_t size() const { return bytes_.size(); }
    std::span<const uint8_t> written_since(size_t offset) const { return std::span(bytes_).subspan(offset); }
    // Reescreve um u64 já escrito em `offset` (campos de cabeçalho preenchidos depois do conteúdo).
    void patch_u64(size_t offset, uint64_t v) {
        for (size_t i = 0; i < 8; ++i) bytes_.at(offset + i) = static_cast<uint8_t>(v >> (8 * i));
    }
    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    template <class T> void put(T v) {
        for (size_t i = 0; i < sizeof(T); ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    std::vector<uint8_t> bytes_;
};

class StateReader {
public:
    explicit StateReader(std::span<const uint8_t> data) : data_(data) {}

    uint8_t u8() { return take(1)[0]; }
    bool boolean() {
        const uint8_t v = u8();
        if (v > 1) throw StateError("save state corrompido: booleano inválido");
        return v;
    }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }
    int16_t i16() { return static_cast<int16_t>(get<uint16_t>()); }
    int32_t i32() { return static_cast<int32_t>(get<uint32_t>()); }
    int64_t i64() { return static_cast<int64_t>(get<uint64_t>()); }
    // Consome e devolve o restante do bloco, sem cópia.
    std::span<const uint8_t> rest() { return take(remaining()); }
    // Copia exatamente out.size() bytes.
    void raw(std::span<uint8_t> out) {
        const auto src = take(out.size());
        if (!out.empty()) std::memcpy(out.data(), src.data(), out.size());
    }
    std::vector<uint8_t> blob() {
        const uint64_t n = u64();
        if (n > remaining()) throw StateError("save state truncado");
        const auto src = take(static_cast<size_t>(n));
        return {src.begin(), src.end()};
    }
    std::string text() {
        const auto bytes = blob();
        return {bytes.begin(), bytes.end()};
    }
    // Valor com um limite superior (contagens de listas): protege contra tamanhos absurdos de um bloco corrompido.
    uint32_t count(uint32_t max) {
        const uint32_t n = u32();
        if (n > max) throw StateError("save state corrompido: contagem fora do limite");
        return n;
    }

    // Lê a seção `tag` (deve ser a próxima) e passa a `body` um leitor restrito ao seu conteúdo, que precisa ser consumido por inteiro.
    template <class F> void section(const char (&tag)[5], F&& body) {
        const auto name = take(4);
        if (std::memcmp(name.data(), tag, 4) != 0) throw StateError(std::string("save state: seção inesperada (esperava ") + tag + ")");
        const uint32_t size = u32();
        StateReader inner(take(size));
        body(inner);
        if (inner.remaining() != 0) throw StateError(std::string("save state corrompido: sobra na seção ") + tag);
    }

    size_t remaining() const { return data_.size() - pos_; }

private:
    std::span<const uint8_t> take(size_t n) {
        if (n > remaining()) throw StateError("save state truncado");
        const auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }
    template <class T> T get() {
        const auto src = take(sizeof(T));
        T v = 0;
        for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(src[i]) << (8 * i));
        return v;
    }
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

}  // namespace saturn
