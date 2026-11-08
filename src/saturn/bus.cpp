#include "saturn/bus.hpp"

#include <algorithm>

#include <stdexcept>

namespace saturn {

struct Bus::Page {
    uint8_t* data = nullptr;          // RAM/ROM: byte do início da página
    IoDevice* device = nullptr;       // E/S
    uint32_t device_offset = 0;       // offset do início da página dentro do dispositivo
    bool read_only = false;
    uint32_t ram_index = UINT32_MAX;  // buffer de RAM física a que a página pertence (para ram_at)
};

struct Bus::Storage {
    static constexpr uint32_t kPageShift = Bus::kPageShift, kPageSize = Bus::kPageSize, kPageMask = Bus::kPageMask;
    static constexpr uint32_t kAddressSpace = 0x20000000;  // após a normalização do cache-through

    std::vector<Page> pages = std::vector<Page>(kAddressSpace >> kPageShift);
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<bool> is_rom;   // paralelo a `buffers`
};

namespace {
using S = Bus::Storage;
constexpr bool is_page_aligned(uint32_t v) { return (v & S::kPageMask) == 0; }

// Normaliza para o espaço físico de 29 bits (os endereços 0x2... são alias de cache-through de 0x0...). Falso fora dele.
constexpr bool to_physical(uint32_t& address) {
    if ((address >> 29) > 1) return false;
    address &= S::kAddressSpace - 1;
    return true;
}
}  // namespace

// Toda remapeação passa aqui: invalida os atalhos de página (page_data) guardados pelas CPUs.
Bus::Page& Bus::map_page(uint32_t address) {
    ++map_generation_;
    ++epoch_;
    return storage_->pages.at(address >> S::kPageShift);
}

Bus::Bus()
    : storage_(std::make_unique<Storage>()),
      fast_read_(std::make_unique<const uint8_t*[]>(S::kAddressSpace >> S::kPageShift)),
      fast_write_(std::make_unique<uint8_t*[]>(S::kAddressSpace >> S::kPageShift)) {}
Bus::~Bus() = default;

void Bus::map_ram(uint32_t base, uint32_t size, uint32_t mirror_span) {
    if (!is_page_aligned(base) || !is_page_aligned(size) || size == 0 || mirror_span < size)
        throw std::invalid_argument("map_ram: base/tamanho precisam ser múltiplos de 4 KiB e mirror_span >= size");
    auto& buffer = storage_->buffers.emplace_back(size, 0);
    storage_->is_rom.push_back(false);
    const auto index = static_cast<uint32_t>(storage_->buffers.size() - 1);
    for (uint32_t offset = 0; offset < mirror_span; offset += S::kPageSize) {
        Page& page = map_page(base + offset);
        page = Page{buffer.data() + offset % size, nullptr, 0, false, index};
        sync_fast(base + offset);
    }
}

void Bus::map_rom(uint32_t base, std::span<const uint8_t> data) {
    if (!is_page_aligned(base)) throw std::invalid_argument("map_rom: base precisa ser múltipla de 4 KiB");
    const uint32_t padded = (static_cast<uint32_t>(data.size()) + S::kPageMask) & ~S::kPageMask;
    auto& buffer = storage_->buffers.emplace_back(padded, 0);
    storage_->is_rom.push_back(true);
    std::copy(data.begin(), data.end(), buffer.begin());
    for (uint32_t offset = 0; offset < padded; offset += S::kPageSize) {
        map_page(base + offset) = Page{buffer.data() + offset, nullptr, 0, true, UINT32_MAX};
        sync_fast(base + offset);
    }
}

void Bus::map_io(uint32_t base, uint32_t size, IoDevice& device) {
    if (!is_page_aligned(base)) throw std::invalid_argument("map_io: base precisa ser múltipla de 4 KiB");
    const uint32_t padded = (size + S::kPageMask) & ~S::kPageMask;
    for (uint32_t offset = 0; offset < padded; offset += S::kPageSize) {
        map_page(base + offset) = Page{nullptr, &device, offset, false, UINT32_MAX};
        sync_fast(base + offset);
    }
}

void Bus::sync_fast(uint32_t address) {
    const uint32_t index = address >> S::kPageShift;
    const Page& page = storage_->pages[index];
    fast_read_[index] = page.data;
    fast_write_[index] = page.read_only ? nullptr : page.data;
}

uint32_t Bus::unmapped(uint32_t address) {
    ++unmapped_count_;
    if (!first_unmapped_) first_unmapped_ = address;
    ++unmapped_pages_[address & ~S::kPageMask];
    return 0;
}

template <int Bytes>
uint32_t Bus::read(uint32_t address) {
    const uint32_t original = address;
    if (!to_physical(address)) return unmapped(original);

    const uint32_t in_page = address & S::kPageMask;
    if (in_page + Bytes > S::kPageSize) {  // acesso desalinhado que cruza a página: byte a byte
        uint32_t value = 0;
        for (int i = 0; i < Bytes; ++i) value = (value << 8) | read<1>(original + i);
        return value;
    }
    const Page& page = storage_->pages[address >> S::kPageShift];
    if (page.data) {
        uint32_t value = 0;
        for (int i = 0; i < Bytes; ++i) value = (value << 8) | page.data[in_page + i];
        return value;
    }
    if (page.device) return page.device->read(page.device_offset + in_page, Bytes);
    return unmapped(original);
}

template <int Bytes>
void Bus::write(uint32_t address, uint32_t value) {
    ++epoch_;
    const uint32_t original = address;
    if (!to_physical(address)) { unmapped(original); return; }

    const uint32_t in_page = address & S::kPageMask;
    if (in_page + Bytes > S::kPageSize) {
        for (int i = 0; i < Bytes; ++i) write<1>(original + i, (value >> (8 * (Bytes - 1 - i))) & 0xFF);
        return;
    }
    const Page& page = storage_->pages[address >> S::kPageShift];
    if (page.data) {
        if (page.read_only) return;
        for (int i = 0; i < Bytes; ++i) page.data[in_page + i] = (value >> (8 * (Bytes - 1 - i))) & 0xFF;
    } else if (page.device) {
        page.device->write(page.device_offset + in_page, Bytes, value);
    } else {
        unmapped(original);
        return;
    }
    if (on_write) on_write(address, Bytes, value);
}

uint8_t Bus::read8(uint32_t address) { return static_cast<uint8_t>(read<1>(address)); }
const Bus::Page* Bus::page_of(uint32_t address) const {
    return to_physical(address) ? &storage_->pages[address >> S::kPageShift] : nullptr;
}

const uint8_t* Bus::page_data(uint32_t address) const {
    const Page* page = page_of(address);
    return page ? page->data : nullptr;
}

uint16_t Bus::read16(uint32_t address) { return static_cast<uint16_t>(read<2>(address)); }
uint32_t Bus::read32(uint32_t address) { return read<4>(address); }
void Bus::write8(uint32_t address, uint8_t value) { write<1>(address, value); }
void Bus::write16(uint32_t address, uint16_t value) { write<2>(address, value); }
void Bus::write32(uint32_t address, uint32_t value) { write<4>(address, value); }

std::span<uint8_t> Bus::ram_at(uint32_t address, size_t length) {
    const Page* page = page_of(address);
    if (!page || page->ram_index == UINT32_MAX) return {};
    auto& buffer = storage_->buffers[page->ram_index];
    const size_t offset = static_cast<size_t>(page->data - buffer.data()) + (address & S::kPageMask);
    if (offset + length > buffer.size()) return {};
    return {buffer.data() + offset, length};
}

// Só as RAMs entram no estado (ROMs vêm do boot); contadores de acesso não mapeado são diagnóstico da sessão e ficam fora.
uint32_t Bus::ram_count() const {
    return static_cast<uint32_t>(std::count(storage_->is_rom.begin(), storage_->is_rom.end(), false));
}

void Bus::save(StateWriter& w) const {
    w.u32(ram_count());
    for (size_t i = 0; i < storage_->buffers.size(); ++i) {
        if (storage_->is_rom[i]) continue;
        w.blob(storage_->buffers[i]);
    }
}

void Bus::load(StateReader& r) {
    if (r.u32() != ram_count()) throw StateError("save state: mapa de RAM diferente (máquina em outro modo de boot?)");
    for (size_t i = 0; i < storage_->buffers.size(); ++i) {
        if (storage_->is_rom[i]) continue;
        auto& buffer = storage_->buffers[i];
        if (r.u64() != buffer.size()) throw StateError("save state: tamanho de RAM diferente");
        r.raw(buffer);
    }
}

void map_standard_memory(Bus& bus) {
    constexpr uint32_t kMegabyte = 0x100000;
    bus.map_ram(0x00200000, kMegabyte, kMegabyte);
    bus.map_ram(0x06000000, kMegabyte, 0x02000000);
}

}  // namespace saturn
