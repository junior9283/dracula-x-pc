// A aceleração de laços ociosos do SH-2 tem que ser invisível: o estado completo da máquina (e, portanto, os ciclos e todos os
// registradores) é idêntico com e sem ela. Um gancho em on_before_master_step desliga a aceleração (precisa ver cada passo).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "iso_builder.hpp"
#include "saturn/machine.hpp"

using namespace testsupport;
using saturn::Machine;

namespace {
constexpr uint32_t kCode = 0x06002100, kWatched = 0x06010000;

std::unique_ptr<Machine> make_machine(const std::string& tag, bool accelerate) {
    SyntheticIso iso("T-9527G", "V1.400");
    iso.add("A.TXT", "alfa");
    auto sectors = iso.sectors();
    sectors[0][0xE2] = 0x18;   // tamanho do IP = 0x1800
    auto disc = std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(write_single_track_disc(make_temp_dir(tag), sectors)));
    auto m = std::make_unique<Machine>(std::move(disc));
    m->boot();
    if (!accelerate) m->on_before_master_step = [](const sh2::Sh2State&) {};   // com observador, nenhuma aceleração
    return m;
}

// Laço de espera: lê a palavra em r1 e repete enquanto ela for zero (tst; bt).
//   mov.l @r1,r2 ; tst r2,r2 ; bt -4 ; nop ; depois, fora do laço: add #1,r3 ; bra . ; nop
void install_wait_loop(Machine& m) {
    const uint16_t program[] = {0x6212, 0x2228, 0x89FC, 0x0009, 0x7301, 0xAFFE, 0x0009};
    for (size_t i = 0; i < std::size(program); ++i) m.bus().write16(kCode + 2 * uint32_t(i), program[i]);
    m.master().state().pc = kCode;
    m.master().state().r[1] = kWatched;
    m.bus().write32(kWatched, 0);
}
}  // namespace

TEST_CASE("laço ocioso do SH-2: o estado é idêntico com e sem aceleração, inclusive depois que a memória observada muda") {
    auto fast = make_machine("idle-fast", true);
    auto slow = make_machine("idle-slow", false);
    install_wait_loop(*fast);
    install_wait_loop(*slow);

    // Várias fatias esperando (a aceleração entra depois das primeiras voltas); fatias de tamanhos que não são múltiplos do laço.
    for (uint64_t cycles : {1000u, 777u, 20000u, 33333u, 100000u, 64u, 5u, 250000u}) {
        fast->run_cycles(cycles);
        slow->run_cycles(cycles);
        REQUIRE(fast->save_state() == slow->save_state());
    }
    CHECK(fast->master().state().r[3] == 0);   // ainda esperando

    // A memória observada muda (escrita pelo barramento): o laço tem que terminar no mesmo ciclo nas duas.
    fast->bus().write32(kWatched, 1);
    slow->bus().write32(kWatched, 1);
    for (uint64_t cycles : {3u, 40u, 5000u}) {
        fast->run_cycles(cycles);
        slow->run_cycles(cycles);
        REQUIRE(fast->save_state() == slow->save_state());
    }
    CHECK(fast->master().state().r[3] > 0);    // saiu do laço e executou o código seguinte
}
