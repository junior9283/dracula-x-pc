#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "sh2/trace.hpp"
#include "sh2_harness.hpp"
#include "support.hpp"

using sh2::ExecutionRecorder;
using testsupport::Sh2Rig;

namespace {
// Programa: nop ; bra +2 (slot = nop) ; nop ; nop ; alvo: nop  — executa 0x00, 0x02, 0x04 (slot) e 0x0A.
void run_program(Sh2Rig& rig, ExecutionRecorder& recorder) {
    rig.cpu.set_recorder(&recorder);
    rig.load({Sh2Rig::kNop, 0xA002, Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop, Sh2Rig::kNop});
    rig.run(3);
}
}  // namespace

TEST_CASE("o gravador marca só os endereços executados, incluindo o delay slot") {
    Sh2Rig rig;
    ExecutionRecorder recorder;
    run_program(rig, recorder);

    CHECK(recorder.executed(0x06000000));
    CHECK(recorder.executed(0x06000002));
    CHECK(recorder.executed(0x06000004));        // slot
    CHECK(recorder.executed(0x0600000A));        // alvo do desvio
    CHECK_FALSE(recorder.executed(0x06000006));  // pulados
    CHECK_FALSE(recorder.executed(0x06000008));
    CHECK(recorder.count() == 4);
}

TEST_CASE("o alias de cache-through (0x26...) é o mesmo endereço") {
    Sh2Rig rig;
    ExecutionRecorder recorder;
    run_program(rig, recorder);

    CHECK(recorder.executed(0x26000000));
    CHECK(recorder.count() == 4);
    recorder.record(0x26000000);                  // repetir não conta de novo
    CHECK(recorder.count() == 4);
}

TEST_CASE("save/load preserva exatamente o conjunto de endereços") {
    Sh2Rig rig;
    ExecutionRecorder recorder;
    run_program(rig, recorder);
    recorder.record(0x00200010);                  // Work RAM-L
    auto file = testsupport::make_temp_dir("trace1") / "trace.bin";

    recorder.save(file);
    auto loaded = ExecutionRecorder::load(file);

    CHECK(loaded.count() == recorder.count());
    for (uint32_t a : {0x06000000u, 0x06000002u, 0x06000004u, 0x0600000Au, 0x00200010u}) CHECK(loaded.executed(a));
    CHECK_FALSE(loaded.executed(0x06000006));
}

TEST_CASE("sem gravador ligado a CPU executa normalmente e nada é registrado") {
    Sh2Rig rig;
    ExecutionRecorder untouched;
    rig.load({Sh2Rig::kNop, Sh2Rig::kNop});
    rig.run(2);
    CHECK(rig.s().pc == Sh2Rig::kCode + 4);
    CHECK(untouched.count() == 0);
}

TEST_CASE("load rejeita arquivo com magic errado ou truncado") {
    auto dir = testsupport::make_temp_dir("trace2");
    testsupport::write_text(dir / "ruim.bin", "NAO-E-UM-TRACO");
    CHECK_THROWS_WITH(ExecutionRecorder::load(dir / "ruim.bin"), Catch::Matchers::ContainsSubstring("traço"));

    Sh2Rig rig;
    ExecutionRecorder recorder;
    run_program(rig, recorder);
    recorder.save(dir / "bom.bin");
    std::filesystem::resize_file(dir / "bom.bin", 10);
    CHECK_THROWS_WITH(ExecutionRecorder::load(dir / "bom.bin"), Catch::Matchers::ContainsSubstring("traço"));
}
