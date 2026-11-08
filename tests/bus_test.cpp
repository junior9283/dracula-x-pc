#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "saturn/bus.hpp"

using saturn::Bus;
using saturn::IoDevice;

namespace {
struct Recorder : IoDevice {
    struct Access { bool write; uint32_t offset; int bytes; uint32_t value; };
    std::vector<Access> log;
    uint32_t read(uint32_t offset, int bytes) override { log.push_back({false, offset, bytes, 0}); return 0xA5A5A5A5u >> (32 - 8 * bytes); }
    void write(uint32_t offset, int bytes, uint32_t value) override { log.push_back({true, offset, bytes, value}); }
};
}  // namespace

TEST_CASE("Work RAM-H: acessos big-endian de 8, 16 e 32 bits") {
    Bus bus;
    saturn::map_standard_memory(bus);

    bus.write32(0x06000000, 0x11223344);

    CHECK(bus.read8(0x06000000) == 0x11);
    CHECK(bus.read8(0x06000003) == 0x44);
    CHECK(bus.read16(0x06000002) == 0x3344);
    CHECK(bus.read32(0x06000000) == 0x11223344u);
}

TEST_CASE("o alias de cache-through (0x26...) acessa a mesma RAM") {
    Bus bus;
    saturn::map_standard_memory(bus);

    bus.write16(0x06000010, 0xBEEF);

    CHECK(bus.read16(0x26000010) == 0xBEEF);
    bus.write8(0x26000011, 0x42);
    CHECK(bus.read16(0x06000010) == 0xBE42);
}

TEST_CASE("Work RAM-H se repete a cada 1 MB até 0x07FFFFFF") {
    Bus bus;
    saturn::map_standard_memory(bus);

    bus.write32(0x06100008, 0xCAFEBABE);

    CHECK(bus.read32(0x06000008) == 0xCAFEBABEu);
    CHECK(bus.read32(0x07F00008) == 0xCAFEBABEu);
}

TEST_CASE("Work RAM-L fica em 0x00200000 e não se repete fora de 1 MB") {
    Bus bus;
    saturn::map_standard_memory(bus);

    bus.write32(0x00200000, 0x01020304);

    CHECK(bus.read32(0x00200000) == 0x01020304u);
    CHECK(bus.read32(0x00300000) == 0u);  // fora da RAM: não mapeado
}

TEST_CASE("ROM ignora escritas") {
    Bus bus;
    const std::vector<uint8_t> rom(0x1000, 0x7E);
    bus.map_rom(0x00000000, rom);

    bus.write32(0x00000000, 0xFFFFFFFF);

    CHECK(bus.read32(0x00000000) == 0x7E7E7E7Eu);
}

TEST_CASE("dispositivo de E/S recebe offset, largura e valor") {
    Bus bus;
    Recorder device;
    bus.map_io(0x05FE0000, 0x100, device);

    bus.write16(0x05FE0010, 0x1234);
    CHECK(bus.read8(0x05FE0021) == 0xA5);
    CHECK(bus.read32(0x05FE0030) == 0xA5A5A5A5u);

    REQUIRE(device.log.size() == 3);
    CHECK(device.log[0].write);  CHECK(device.log[0].offset == 0x10);  CHECK(device.log[0].bytes == 2);  CHECK(device.log[0].value == 0x1234u);
    CHECK_FALSE(device.log[1].write);  CHECK(device.log[1].offset == 0x21);  CHECK(device.log[1].bytes == 1);
    CHECK(device.log[2].offset == 0x30);  CHECK(device.log[2].bytes == 4);
}

TEST_CASE("endereço não mapeado lê 0, é contado e o primeiro fica registrado") {
    Bus bus;
    saturn::map_standard_memory(bus);

    CHECK(bus.read32(0x04000000) == 0u);
    CHECK(bus.read8(0x04000010) == 0u);

    CHECK(bus.unmapped_count() == 2);
    REQUIRE(bus.first_unmapped().has_value());
    CHECK(*bus.first_unmapped() == 0x04000000u);
}

TEST_CASE("ram_at devolve o trecho contíguo da RAM e vazio fora dela") {
    Bus bus;
    saturn::map_standard_memory(bus);
    bus.write8(0x06000020, 0x99);

    auto span = bus.ram_at(0x06000020, 16);
    REQUIRE(span.size() == 16);
    CHECK(span[0] == 0x99);
    CHECK(bus.ram_at(0x06000020, 0x200000).empty());  // atravessa o fim da RAM física
    CHECK(bus.ram_at(0x04000000, 4).empty());
}

TEST_CASE("acessos não mapeados são contados por página de 4 KiB (leitura e escrita)") {
    Bus bus;
    saturn::map_standard_memory(bus);

    bus.read32(0x05D00000);
    bus.write16(0x05D00004, 1);
    bus.read8(0x05D00FFF);
    bus.read32(0x05F80000);

    const auto& pages = bus.unmapped_pages();
    REQUIRE(pages.size() == 2);
    CHECK(pages.at(0x05D00000) == 3u);
    CHECK(pages.at(0x05F80000) == 1u);
}

TEST_CASE("observador de escrita vê RAM, alias físico e E/S, mas ignora ROM e endereços não mapeados") {
    Bus bus;
    saturn::map_standard_memory(bus);
    Recorder device;
    bus.map_io(0x05FE0000, 0x1000, device);
    bus.map_rom(0, std::vector<uint8_t>(0x1000));
    struct Write { uint32_t address; int bytes; uint32_t value; };
    std::vector<Write> writes;
    bus.on_write = [&](uint32_t address, int bytes, uint32_t value) { writes.push_back({address, bytes, value}); };

    bus.write32(0x26000010, 0x12345678);
    bus.write16(0x05FE0010, 0xABCD);
    bus.write8(0x00000000, 0xAA);
    bus.write8(0x04000000, 0xBB);

    REQUIRE(writes.size() == 2);
    CHECK(writes[0].address == 0x06000010u);
    CHECK(writes[0].bytes == 4);
    CHECK(writes[0].value == 0x12345678u);
    CHECK(writes[1].address == 0x05FE0010u);
    CHECK(writes[1].bytes == 2);
    CHECK(writes[1].value == 0xABCDu);
}
