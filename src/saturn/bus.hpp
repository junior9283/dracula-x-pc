#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "saturn/state.hpp"

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
    static constexpr uint32_t kPageShift = 12, kPageSize = 1u << kPageShift, kPageMask = kPageSize - 1;  // páginas de 4 KiB

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

    // Diagnóstico opcional: uma chamada após cada escrita efetiva pelo barramento.
    // O endereço é físico (aliases 0x2... normalizados); escritas diretas via ram_at não passam aqui.
    std::function<void(uint32_t address, int bytes, uint32_t value)> on_write;

    // Busca rápida de instruções: bytes da página de 4 KiB de RAM/ROM que contém `address` (nullptr se for E/S ou não mapeada).
    // O ponteiro vale enquanto map_generation() não mudar.
    const uint8_t* page_data(uint32_t address) const;
    uint32_t map_generation() const { return map_generation_; }

    // Trecho contíguo da RAM física a partir de `address`; vazio se não for RAM ou se atravessar o fim dela.
    std::span<uint8_t> ram_at(uint32_t address, size_t length);

    uint64_t unmapped_count() const { return unmapped_count_; }
    std::optional<uint32_t> first_unmapped() const { return first_unmapped_; }
    // Acessos não mapeados por página de 4 KiB (endereço físico do início da página).
    const std::map<uint32_t, uint64_t>& unmapped_pages() const { return unmapped_pages_; }

    // Save state: conteúdo de todas as RAMs mapeadas (as ROMs não entram) e contadores de acessos não mapeados. O mapeamento em si é
    // estrutural (feito na construção/boot) e não é serializado; load exige o mesmo mapeamento.
    void save(StateWriter& w) const;
    void load(StateReader& r);
    uint32_t ram_count() const;   // buffers de RAM (os que entram no save state)

    struct Page;
    struct Storage;

private:
    const Page* page_of(uint32_t address) const;  // nullptr fora do espaço físico
    Page& map_page(uint32_t address);             // entrada da página para (re)mapear; invalida os atalhos
    uint32_t unmapped(uint32_t address);  // contabiliza o acesso e devolve 0
    template <int Bytes> uint32_t read(uint32_t address);
    template <int Bytes> void write(uint32_t address, uint32_t value);

    std::unique_ptr<Storage> storage_;
    uint64_t unmapped_count_ = 0;
    std::optional<uint32_t> first_unmapped_;
    uint32_t map_generation_ = 0;
    std::map<uint32_t, uint64_t> unmapped_pages_;
};

// Work RAM-L (1 MB em 0x00200000) e Work RAM-H (1 MB em 0x06000000, repetida até 0x07FFFFFF).
void map_standard_memory(Bus& bus);

}  // namespace saturn
