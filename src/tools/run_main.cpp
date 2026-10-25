#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "saturn/machine.hpp"
#include "saturnfs/image.hpp"
#include "saturnfs/verify.hpp"

namespace {

std::string hex(uint32_t v) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08X", v);
    return buffer;
}

void print_state(saturn::Machine& m) {
    const auto& s = m.master().state();
    std::cout << "mestre: PC=" << hex(s.pc) << " PR=" << hex(s.pr) << " SR=" << hex(s.sr) << " SP=" << hex(s.r[15]) << '\n';
    for (int i = 0; i < 16; i += 4)
        std::cout << "  r" << i << "=" << hex(s.r[i]) << " r" << i + 1 << "=" << hex(s.r[i + 1]) << " r" << i + 2 << "=" << hex(s.r[i + 2])
                  << " r" << i + 3 << "=" << hex(s.r[i + 3]) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: dxpc-run <arquivo.cue> [--frames N] [--trace arquivo] [--lenient-bios]\n";
        return 2;
    }
    int frames = 5;
    std::string trace_file;
    bool lenient = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) trace_file = argv[++i];
        else if (!std::strcmp(argv[i], "--lenient-bios")) lenient = true;
        else { std::cerr << "argumento desconhecido: " << argv[i] << '\n'; return 2; }
    }

    try {
        auto disc = std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(argv[1]));
        saturnfs::require_supported_disc(saturnfs::read_header(*disc));
        saturnfs::require_complete_image(*disc);

        saturn::Machine machine(std::move(disc));
        machine.set_bios_strict(!lenient);
        machine.boot();
        int completed = 0;
        try {
            for (; completed < frames; ++completed) machine.run_cycles(saturn::VideoTiming::kCyclesPerFrame);
        } catch (const saturn::MachineError& e) {
            std::cout << "PAROU no quadro " << machine.frame() << ": " << e.what() << '\n';
            print_state(machine);
        }

        std::cout << "quadros concluídos: " << completed << " (de " << frames << ")\n";
        std::cout << "endereços de instrução distintos executados: " << machine.recorder().count() << '\n';
        if (auto first = machine.bus().first_unmapped()) {
            std::cout << "acessos a endereço não mapeado: " << machine.bus().unmapped_count() << " (primeiro: " << hex(*first) << ")\n";
            std::vector<std::pair<uint64_t, uint32_t>> pages;
            for (const auto& [page, count] : machine.bus().unmapped_pages()) pages.emplace_back(count, page);
            std::sort(pages.rbegin(), pages.rend());
            for (size_t i = 0; i < pages.size() && i < 12; ++i) std::cout << "  página " << hex(pages[i].second) << ": " << pages[i].first << " acessos\n";
        }
        print_state(machine);
        std::map<uint32_t, std::pair<size_t, saturn::BiosHle::Call>> by_slot;   // contagem e a primeira chamada de cada slot
        for (const auto& call : machine.bios_calls()) {
            auto& entry = by_slot.try_emplace(call.slot, 0, call).first->second;
            ++entry.first;
        }
        for (const auto& [slot, entry] : by_slot)
            std::cout << "serviço do BIOS (tolerante) slot " << hex(slot) << ": " << entry.first << " chamada(s); 1ª de " << hex(entry.second.return_address)
                      << " r4=" << hex(entry.second.args[0]) << " r5=" << hex(entry.second.args[1]) << " r6=" << hex(entry.second.args[2]) << '\n';
        for (uint8_t cmd : machine.cd().unknown_commands()) std::cout << "comando de CD block desconhecido: " << hex(cmd) << '\n';
        for (uint8_t cmd : machine.smpc().unknown_commands()) std::cout << "comando de SMPC desconhecido: " << hex(cmd) << '\n';
        if (!trace_file.empty()) {
            machine.recorder().save(trace_file);
            std::cout << "traço gravado em " << trace_file << '\n';
        }
        return completed == frames ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << '\n';
        return 1;
    }
}
