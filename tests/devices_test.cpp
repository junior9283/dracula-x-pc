#include <catch2/catch_test_macros.hpp>

#include "saturn/devices.hpp"

using saturn::ConstantDevice;
using saturn::StorageDevice;

TEST_CASE("StorageDevice guarda e devolve o que foi escrito, em qualquer largura (big-endian)") {
    StorageDevice device(0x100);

    device.write(0x10, 4, 0x11223344);
    CHECK(device.read(0x10, 4) == 0x11223344u);
    CHECK(device.read(0x10, 2) == 0x1122u);
    CHECK(device.read(0x12, 2) == 0x3344u);
    CHECK(device.read(0x13, 1) == 0x44u);

    device.write(0x11, 1, 0xAA);
    CHECK(device.read(0x10, 4) == 0x11AA3344u);
}

TEST_CASE("StorageDevice: valores iniciais e fora do tamanho") {
    StorageDevice device(0x20);
    device.set16(0x10, 0x0002);
    CHECK(device.read(0x10, 2) == 2u);
    CHECK(device.read(0x40, 4) == 0u);        // fora do armazenamento: lê 0
    device.write(0x40, 4, 1);                  // e a escrita é ignorada (sem estourar)
    CHECK(device.read(0x40, 4) == 0u);
}

TEST_CASE("ConstantDevice devolve sempre o mesmo byte e ignora escritas") {
    ConstantDevice device(0xFF);
    device.write(0, 4, 0);
    CHECK(device.read(0x1000, 1) == 0xFFu);
    CHECK(device.read(0x20, 2) == 0xFFFFu);
    CHECK(device.read(0x20, 4) == 0xFFFFFFFFu);
}
