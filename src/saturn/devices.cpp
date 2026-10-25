#include "saturn/devices.hpp"

namespace saturn {

uint32_t StorageDevice::read(uint32_t offset, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | (offset + i < bytes_.size() ? bytes_[offset + i] : 0);
    return value;
}

void StorageDevice::write(uint32_t offset, int bytes, uint32_t value) {
    for (int i = 0; i < bytes; ++i)
        if (offset + i < bytes_.size()) bytes_[offset + i] = (value >> (8 * (bytes - 1 - i))) & 0xFF;
}

uint32_t SoundMailboxStub::read(uint32_t offset, int bytes) {
    if (offset < kMailboxEnd && offset + bytes > kMailboxBegin) return 0;
    return StorageDevice::read(offset, bytes);
}

void StorageDevice::set16(uint32_t offset, uint16_t value) { write(offset, 2, value); }

}  // namespace saturn
