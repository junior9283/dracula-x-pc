#include "saturn/bus.hpp"

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
    static constexpr uint32_t kPageShift = 12;
    static constexpr uint32_t kPageSize = 1u << kPageShift;
    static constexpr uint32_t kPageMask = kPageSize - 1;
    static constexpr uint32_t kAddressSpace = 0x20000000;  // após a normalização do cache-through

    std::vector<Page> pages = std::vector<Page>(kAddressSpace >> kPageShift);
    std::vector<std::vector<uint8_t>> buffers;
};

namespace {
using S = Bus::Storage;
constexpr bool is_page_aligned(uint32_t v) { return (v & S::kPageMask) == 0; }
}  // namespace

Bus::Bus() : storage_(std::make_unique<Storage>()) {}
Bus::~Bus() = default;

void Bus::map_ram(uint32_t base, uint32_t size, uint32_t mirror_span) {
    if (!is_page_aligned(base) || !is_page_aligned(size) || size == 0 || mirror_span < size)
        throw std::invalid_argument("map_ram: base/tamanho precisam ser múltiplos de 4 KiB e mirror_span >= size");
    auto& buffer = storage_->buffers.emplace_back(size, 0);
    const auto index = static_cast<uint32_t>(storage_->buffers.size() - 1);
    for (uint32_t offset = 0; offset < mirror_span; offset += S::kPageSize) {
        Page& page = storage_->pages.at((base + offset) >> S::kPageShift);
        page = Page{buffer.data() + offset % size, nullptr, 0, false, index};
    }
}

void Bus::map_rom(uint32_t base, std::span<const uint8_t> data) {
    if (!is_page_aligned(base)) throw std::invalid_argument("map_rom: base precisa ser múltipla de 4 KiB");
    const uint32_t padded = (static_cast<uint32_t>(data.size()) + S::kPageMask) & ~S::kPageMask;
    auto& buffer = storage_->buffers.emplace_back(padded, 0);
    std::copy(data.begin(), data.end(), buffer.begin());
    for (uint32_t offset = 0; offset < padded; offset += S::kPageSize)
        storage_->pages.at((base + offset) >> S::kPageShift) = Page{buffer.data() + offset, nullptr, 0, true, UINT32_MAX};
}

void Bus::map_io(uint32_t base, uint32_t size, IoDevice& device) {
    if (!is_page_aligned(base)) throw std::invalid_argument("map_io: base precisa ser múltipla de 4 KiB");
    const uint32_t padded = (size + S::kPageMask) & ~S::kPageMask;
    for (uint32_t offset = 0; offset < padded; offset += S::kPageSize)
        storage_->pages.at((base + offset) >> S::kPageShift) = Page{nullptr, &device, offset, false, UINT32_MAX};
}

template <int Bytes>
uint32_t Bus::read(uint32_t address) {
    if ((address >> 29) <= 1) address &= S::kAddressSpace - 1;
    else address = UINT32_MAX;  // fora do espaço mapeável

    const uint32_t in_page = address & S::kPageMask;
    if (address != UINT32_MAX && in_page + Bytes > S::kPageSize) {  // acesso desalinhado que cruza a página: byte a byte
        uint32_t value = 0;
        for (int i = 0; i < Bytes; ++i) value = (value << 8) | read<1>(address + i);
        return value;
    }
    const Page* page = address == UINT32_MAX ? nullptr : &storage_->pages[address >> S::kPageShift];
    if (page && page->data) {
        uint32_t value = 0;
        for (int i = 0; i < Bytes; ++i) value = (value << 8) | page->data[in_page + i];
        return value;
    }
    if (page && page->device) return page->device->read(page->device_offset + in_page, Bytes);

    ++unmapped_count_;
    if (!first_unmapped_) first_unmapped_ = address;
    return 0;
}

template <int Bytes>
void Bus::write(uint32_t address, uint32_t value) {
    if ((address >> 29) <= 1) address &= S::kAddressSpace - 1;
    else address = UINT32_MAX;

    const uint32_t in_page = address & S::kPageMask;
    if (address != UINT32_MAX && in_page + Bytes > S::kPageSize) {
        for (int i = 0; i < Bytes; ++i) write<1>(address + i, (value >> (8 * (Bytes - 1 - i))) & 0xFF);
        return;
    }
    const Page* page = address == UINT32_MAX ? nullptr : &storage_->pages[address >> S::kPageShift];
    if (page && page->data) {
        if (page->read_only) return;
        for (int i = 0; i < Bytes; ++i) page->data[in_page + i] = (value >> (8 * (Bytes - 1 - i))) & 0xFF;
        return;
    }
    if (page && page->device) {
        page->device->write(page->device_offset + in_page, Bytes, value);
        return;
    }
    ++unmapped_count_;
    if (!first_unmapped_) first_unmapped_ = address;
}

uint8_t Bus::read8(uint32_t address) { return static_cast<uint8_t>(read<1>(address)); }
uint16_t Bus::read16(uint32_t address) { return static_cast<uint16_t>(read<2>(address)); }
uint32_t Bus::read32(uint32_t address) { return read<4>(address); }
void Bus::write8(uint32_t address, uint8_t value) { write<1>(address, value); }
void Bus::write16(uint32_t address, uint16_t value) { write<2>(address, value); }
void Bus::write32(uint32_t address, uint32_t value) { write<4>(address, value); }

std::span<uint8_t> Bus::ram_at(uint32_t address, size_t length) {
    if ((address >> 29) <= 1) address &= S::kAddressSpace - 1;
    else return {};
    const Page& page = storage_->pages[address >> S::kPageShift];
    if (page.ram_index == UINT32_MAX) return {};
    auto& buffer = storage_->buffers[page.ram_index];
    const size_t offset = static_cast<size_t>(page.data - buffer.data()) + (address & S::kPageMask);
    if (offset + length > buffer.size()) return {};
    return {buffer.data() + offset, length};
}

void map_standard_memory(Bus& bus) {
    constexpr uint32_t kMegabyte = 0x100000;
    bus.map_ram(0x00200000, kMegabyte, kMegabyte);
    bus.map_ram(0x06000000, kMegabyte, 0x02000000);
}

}  // namespace saturn
