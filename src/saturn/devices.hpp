#pragma once
#include <cstdint>
#include <vector>

#include "saturn/bus.hpp"

namespace saturn {

// Banco de registradores simples: guarda o que foi escrito (big-endian, qualquer largura). Fora do tamanho: lê 0 e ignora escritas.
class StorageDevice : public IoDevice {
public:
    explicit StorageDevice(size_t size) : bytes_(size, 0) {}

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;
    void set16(uint32_t offset, uint16_t value);  // valor inicial de um registrador de 16 bits

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

// PROVISÓRIO (substituído pelo 68000 na etapa 4): primeira página da RAM de som, onde o SH-2 deixa comandos para o driver. Sem o
// driver, a caixa de correio (0x700–0x70F) sempre lê zero, ou seja, todo comando é visto como já aceito.
class SoundMailboxStub : public StorageDevice {
public:
    static constexpr uint32_t kMailboxBegin = 0x700, kMailboxEnd = 0x710;
    SoundMailboxStub() : StorageDevice(0x1000) {}
    uint32_t read(uint32_t offset, int bytes) override;
};

}  // namespace saturn
