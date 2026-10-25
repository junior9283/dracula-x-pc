#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace saturn {

// Dispositivo mapeado em memória. `offset` é relativo à base com que foi mapeado; `bytes` é 1, 2 ou 4.
class IoDevice {
public:
    virtual ~IoDevice() = default;
    virtual uint32_t read(uint32_t offset, int bytes) = 0;
    virtual void write(uint32_t offset, int bytes, uint32_t value) = 0;
};

// Barramento do Saturn: memória big-endian, páginas de 4 KiB. Endereços com bits 31–29 = 000 ou 001 (cache-through)
// são normalizados para 0x1FFFFFFF; o espaço de E/S interna do SH-2 (0xFFFFFE00+) é tratado pela CPU, não aqui.
class Bus {
public:
    Bus();
    ~Bus();

    // RAM de `size` bytes visível a partir de `base`, repetida (espelhada) até cobrir `mirror_span` bytes.
    void map_ram(uint32_t base, uint32_t size, uint32_t mirror_span);
    void map_rom(uint32_t base, std::span<const uint8_t> data);
    void map_io(uint32_t base, uint32_t size, IoDevice& device);

    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    // Trecho contíguo da RAM física a partir de `address`; vazio se não for RAM ou se atravessar o fim dela.
    std::span<uint8_t> ram_at(uint32_t address, size_t length);

    uint64_t unmapped_count() const { return unmapped_count_; }
    std::optional<uint32_t> first_unmapped() const { return first_unmapped_; }
    // Acessos não mapeados por página de 4 KiB (endereço físico do início da página).
    const std::map<uint32_t, uint64_t>& unmapped_pages() const { return unmapped_pages_; }

    struct Page;
    struct Storage;

private:
    uint32_t unmapped(uint32_t address);  // contabiliza o acesso e devolve 0
    template <int Bytes> uint32_t read(uint32_t address);
    template <int Bytes> void write(uint32_t address, uint32_t value);

    std::unique_ptr<Storage> storage_;
    uint64_t unmapped_count_ = 0;
    std::optional<uint32_t> first_unmapped_;
    std::map<uint32_t, uint64_t> unmapped_pages_;
};

// Work RAM-L (1 MB em 0x00200000) e Work RAM-H (1 MB em 0x06000000, repetida até 0x07FFFFFF).
void map_standard_memory(Bus& bus);

}  // namespace saturn
