#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "audio/scsp.hpp"
#include "saturn/bus.hpp"
#include "saturn/state.hpp"

namespace saturn {

// Banco de registradores simples: guarda o que foi escrito (big-endian, qualquer largura). Fora do tamanho: lê 0 e ignora escritas.
class StorageDevice : public IoDevice {
public:
    explicit StorageDevice(size_t size) : bytes_(size, 0) {}

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;
    void set16(uint32_t offset, uint16_t value);  // valor inicial de um registrador de 16 bits
    void save(StateWriter& w) const { w.raw(bytes_); }
    void load(StateReader& r) { r.raw(bytes_); }   // tamanho fixo: diferença aparece como sobra/truncamento da seção

private:
    std::vector<uint8_t> bytes_;
};

// Região que sempre lê o mesmo byte (ex.: A-bus sem cartucho lê 0xFF) e ignora escritas.
class ConstantDevice : public IoDevice {
public:
    explicit ConstantDevice(uint8_t value) : value_(value) {}
    uint32_t read(uint32_t, int bytes) override { return bytes == 1 ? value_ : bytes == 2 ? value_ * 0x0101u : value_ * 0x01010101u; }
    void write(uint32_t, int, uint32_t) override {}

private:
    uint8_t value_;
};

// MINIT (0x01800000, avisa o mestre) e SINIT (0x01000000, avisa o escravo): qualquer escrita dispara `on_write`; a leitura devolve zero.
class InitSignal : public IoDevice {
public:
    std::function<void()> on_write;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int, uint32_t) override { if (on_write) on_write(); }
};

// Janelas do SCSP vistas pelo SH-2: a RAM de som (0x25A00000, 512 KiB espelhada) e os registradores (0x25B00000).
class ScspRamWindow : public IoDevice {
public:
    explicit ScspRamWindow(audio::Scsp& scsp) : scsp_(scsp) {}
    uint32_t read(uint32_t offset, int bytes) override { return scsp_.read_ram(offset, bytes); }
    void write(uint32_t offset, int bytes, uint32_t value) override { scsp_.write_ram(offset, bytes, value); }

private:
    audio::Scsp& scsp_;
};

class ScspRegisterWindow : public IoDevice {
public:
    explicit ScspRegisterWindow(audio::Scsp& scsp) : scsp_(scsp) {}
    uint32_t read(uint32_t offset, int bytes) override { return scsp_.read_reg(offset, bytes); }
    void write(uint32_t offset, int bytes, uint32_t value) override { scsp_.write_reg(offset, bytes, value); }

private:
    audio::Scsp& scsp_;
};

}  // namespace saturn
