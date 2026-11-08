#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "frontend/fileio.hpp"
#include "frontend/machine_video.hpp"
#include "audio/scsp.hpp"
#include "audio/wav.hpp"
#include "saturn/machine.hpp"
#include "saturnfs/image.hpp"
#include "saturnfs/verify.hpp"
#include "video/vdp1.hpp"
#include "video/vdp2.hpp"

namespace {

std::string hex(uint32_t v) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08X", v);
    return buffer;
}

void print_cpu(const char* name, const sh2::Sh2State& s) {
    std::cout << name << ": PC=" << hex(s.pc) << " PR=" << hex(s.pr) << " SR=" << hex(s.sr) << " SP=" << hex(s.r[15]) << '\n';
    for (int i = 0; i < 16; i += 4)
        std::cout << "  r" << i << "=" << hex(s.r[i]) << " r" << i + 1 << "=" << hex(s.r[i + 1]) << " r" << i + 2 << "=" << hex(s.r[i + 2])
                  << " r" << i + 3 << "=" << hex(s.r[i + 3]) << '\n';
}

void print_state(saturn::Machine& m) {
    print_cpu("mestre", m.master().state());
    if (m.slave_running()) print_cpu("escravo", m.slave().state());
    if (!m.slave_fault().empty()) std::cout << "escravo parou: " << m.slave_fault() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: dxpc-run <arquivo.cue> [--frames N] [--trace arquivo] [--coverage] [--lenient-bios] [--peek endereço]... [--watch-write endereço]... [--stop-on-write endereço=valor] [--bios arquivo (só desenvolvimento: a BIOS é do próprio usuário)] [--boot-font arquivo (4096 bytes da RAM descompactada pelo BIOS)] [--until-pc endereço] [--log-cd N] [--log-cd-context] [--history-on-cd-reject] [--stop-on-unmapped] [--dump-ram arquivo (1 MB da Work RAM-H, uso local)] [--screenshot arquivo.png (VDP2 ao fim da execução)] [--pad quadro:máscara (botões pressionados a partir do quadro; Start = 0x0800)] [--poke quadro:endereço=valor (escrita de 16 bits no início do quadro)] [--frame-log arquivo.csv (por quadro: microssegundos de emulação e de desenho do vídeo, como no executável)] [--sample-pc quadro] [--shot quadro:arquivo.png (repetível)] [--vdp1-log de:até (nº de pixels de sprite por quadro)] [--flow-frame quadro (lista, em ordem, os PCs novos executados no quadro)] [--layers máscara: bits 0-3 NBG0..3, bit 4 sprites, bit 5 RBG0] [--dump-video prefixo (VRAM/CRAM/registradores do VDP1 e VDP2, uso local)] [--history N] [--history-from-frame N] [--wav arquivo.wav (saída do SCSP, 44,1 kHz estéreo)] [--save-state quadro:arquivo (repetível; grava o estado ao fim daquele quadro, depois das capturas)] [--load-state arquivo (carrega após o boot, antes do primeiro quadro; --frames/--shot/--pad/--save-state passam a contar a partir do frame() da máquina carregada)]\n";
        return 2;
    }
    int frames = 5;
    std::string trace_file;
    bool coverage = false;  // conta os endereços de instrução distintos (implícito com --trace)
    bool lenient = false;
    std::vector<uint32_t> peeks;
    std::vector<uint32_t> watched_writes;
    std::optional<std::pair<uint32_t, uint32_t>> stop_on_write;  // endereço físico e valor que interrompem a execução
    std::string bios_file, boot_font_file, dump_ram_file, dump_video_prefix, screenshot_file, wav_file;
    unsigned layer_mask = 0x3F;
    std::vector<std::pair<int, std::string>> shots;   // capturas em quadros específicos
    std::vector<std::pair<int, std::string>> save_states;   // estados gravados ao fim de quadros específicos
    std::string load_state_file;
    int sample_frame = -1, flow_frame = -1, vdp1_log_from = -1, vdp1_log_to = -1;
    struct Poke { int frame; uint32_t address; uint16_t value; };
    std::vector<Poke> pokes;   // escritas de 16 bits no início de um quadro (diagnóstico: trocar fase, flags...)
    std::string frame_log_file;   // --frame-log: tempo de emulação e de desenho de cada quadro (CSV)
    std::vector<std::pair<int, uint16_t>> pad_events;   // (quadro, estado do controle 1: bit 0 = pressionado)
    uint32_t until_pc = 0;
    int log_cd = 0;
    bool log_cd_context = false;
    bool history_on_cd_reject = false, stop_on_unmapped = false;
    size_t history_size = 0;
    uint64_t history_from_frame = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) trace_file = argv[++i];
        else if (!std::strcmp(argv[i], "--coverage")) coverage = true;
        else if (!std::strcmp(argv[i], "--lenient-bios")) lenient = true;
        else if (!std::strcmp(argv[i], "--dump-ram") && i + 1 < argc) dump_ram_file = argv[++i];
        else if (!std::strcmp(argv[i], "--pad") && i + 1 < argc) {
            char* end = nullptr;
            const int frame = static_cast<int>(std::strtol(argv[++i], &end, 0));
            if (*end != ':') throw std::invalid_argument("--pad: use quadro:máscara (bits 1 = pressionado: 0x0800 Start, 0x1000 Cima…)");
            pad_events.emplace_back(frame, static_cast<uint16_t>(std::strtoul(end + 1, nullptr, 0)));
        }
        else if (!std::strcmp(argv[i], "--frame-log") && i + 1 < argc) frame_log_file = argv[++i];
        else if (!std::strcmp(argv[i], "--poke") && i + 1 < argc) {
            char* end = nullptr;
            Poke poke{};
            poke.frame = static_cast<int>(std::strtol(argv[++i], &end, 0));
            if (*end != ':') throw std::invalid_argument("--poke: use quadro:endereço=valor");
            poke.address = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
            if (*end != '=') throw std::invalid_argument("--poke: use quadro:endereço=valor");
            poke.value = static_cast<uint16_t>(std::strtoul(end + 1, nullptr, 0));
            pokes.push_back(poke);
        }
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) throw std::invalid_argument("--shot: use quadro:arquivo.png");
            shots.emplace_back(std::stoi(spec.substr(0, colon)), spec.substr(colon + 1));
        }
        else if (!std::strcmp(argv[i], "--save-state") && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) throw std::invalid_argument("--save-state: use quadro:arquivo");
            save_states.emplace_back(std::stoi(spec.substr(0, colon)), spec.substr(colon + 1));
        }
        else if (!std::strcmp(argv[i], "--load-state") && i + 1 < argc) load_state_file = argv[++i];
        else if (!std::strcmp(argv[i], "--vdp1-log") && i + 1 < argc) {
            const char* range = argv[++i];
            char* end = nullptr;
            vdp1_log_from = static_cast<int>(std::strtol(range, &end, 0));
            vdp1_log_to = *end == ':' ? static_cast<int>(std::strtol(end + 1, nullptr, 0)) : vdp1_log_from;
        }
        else if (!std::strcmp(argv[i], "--flow-frame") && i + 1 < argc) flow_frame = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sample-pc") && i + 1 < argc) sample_frame = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--layers") && i + 1 < argc) layer_mask = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot_file = argv[++i];
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wav_file = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-video") && i + 1 < argc) dump_video_prefix = argv[++i];
        else if (!std::strcmp(argv[i], "--log-cd") && i + 1 < argc) log_cd = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--log-cd-context")) log_cd_context = true;
        else if (!std::strcmp(argv[i], "--history-on-cd-reject")) history_on_cd_reject = true;
        else if (!std::strcmp(argv[i], "--stop-on-unmapped")) stop_on_unmapped = true;
        else if (!std::strcmp(argv[i], "--history") && i + 1 < argc) history_size = std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--history-from-frame") && i + 1 < argc) history_from_frame = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--bios") && i + 1 < argc) bios_file = argv[++i];
        else if (!std::strcmp(argv[i], "--boot-font") && i + 1 < argc) boot_font_file = argv[++i];
        else if (!std::strcmp(argv[i], "--until-pc") && i + 1 < argc) until_pc = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--peek") && i + 1 < argc) peeks.push_back(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
        else if (!std::strcmp(argv[i], "--stop-on-write") && i + 1 < argc) {
            char* end = nullptr;
            const auto address = static_cast<uint32_t>(std::strtoul(argv[++i], &end, 0));
            if (*end != '=') throw std::invalid_argument("--stop-on-write: use endereço=valor");
            stop_on_write = {address & 0x1FFFFFFF, static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0))};
        }
        else if (!std::strcmp(argv[i], "--watch-write") && i + 1 < argc) watched_writes.push_back(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)) & 0x1FFFFFFF);
        else { std::cerr << "argumento desconhecido: " << argv[i] << '\n'; return 2; }
    }

    try {
        auto disc = std::make_unique<saturnfs::DiscImage>(saturnfs::DiscImage::open(argv[1]));
        saturnfs::require_supported_disc(saturnfs::read_header(*disc));
        saturnfs::require_complete_image(*disc);

        saturn::Machine machine(std::move(disc));
        if (!trace_file.empty()) coverage = true;
        machine.record_execution(coverage);
        machine.set_bios_strict(!lenient);
        if (!watched_writes.empty() || stop_on_write) machine.bus().on_write = [&](uint32_t address, int bytes, uint32_t value) {
            if (stop_on_write && machine.frame() >= history_from_frame && (address & 0x1FFFFFFF) == stop_on_write->first && value == stop_on_write->second)
                throw saturn::MachineError("--stop-on-write: " + hex(address) + " = " + hex(value));
            for (uint32_t watched : watched_writes) {
                if (watched < address || uint64_t(watched) >= uint64_t(address) + bytes) continue;
                const auto& s = machine.master().state();
                std::cout << "escrita observada: quadro=" << machine.frame() << " PC=" << hex(s.pc)
                          << " endereço=" << hex(address) << " bytes=" << bytes << " valor=" << hex(value)
                          << " r0=" << hex(s.r[0]) << " r1=" << hex(s.r[1]) << " r2=" << hex(s.r[2])
                          << " | escravo PC=" << hex(machine.slave().state().pc) << " PR=" << hex(machine.slave().state().pr) << '\n';
                break;
            }
        };
        struct StepEntry {
            uint32_t pc, opcode, pr, sr;
            std::array<uint32_t, 16> registers;
        };
        if (history_size > 4096) throw std::invalid_argument("--history: máximo 4096 passos");
        std::vector<StepEntry> history(history_size);
        size_t history_next = 0, history_count = 0;
        if (history_size || stop_on_unmapped) machine.on_before_master_step = [&](const sh2::Sh2State& state) {
            if (machine.frame() < history_from_frame) return;
            if (stop_on_unmapped && machine.bus().unmapped_count() > 0)
                throw saturn::MachineError("--stop-on-unmapped: primeiro acesso em " + hex(*machine.bus().first_unmapped()));
            if (!history_size) return;
            uint32_t opcode = std::numeric_limits<uint32_t>::max();
            if (state.pc >= 0x06000000 && state.pc < 0x08000000)
                opcode = machine.bus().read16(state.pc);
            StepEntry& entry = history[history_next];
            entry.pc = state.pc;
            entry.opcode = opcode;
            entry.pr = state.pr;
            entry.sr = state.sr;
            std::copy(std::begin(state.r), std::end(state.r), entry.registers.begin());
            history_next = (history_next + 1) % history.size();
            history_count = std::min(history_count + 1, history.size());
        };
        auto print_history = [&] {
            std::cout << "últimos " << history_count << " passos do mestre (antes de executar):\n";
            for (size_t i = 0; i < history_count; ++i) {
                const StepEntry& entry = history[(history_next + history.size() - history_count + i) % history.size()];
                std::cout << "  PC=" << hex(entry.pc) << " op=" << hex(entry.opcode) << " PR=" << hex(entry.pr) << " SR=" << hex(entry.sr);
                for (int r = 0; r < 16; ++r) std::cout << " r" << r << "=" << hex(entry.registers[r]);
                std::cout << '\n';
            }
        };
        if (!boot_font_file.empty()) {
            machine.set_boot_font(frontend::read_file_bytes(boot_font_file));
        }
        int logged_cd = 0;
        machine.cd().on_command = [&](const std::array<uint16_t, 4>& c, const std::array<uint16_t, 4>& r) {
            if (logged_cd++ >= log_cd) return;
            char line[160];
            std::snprintf(line, sizeof line, "CD cmd %04X %04X %04X %04X -> %04X %04X %04X %04X (quadro %llu)", c[0], c[1], c[2], c[3], r[0], r[1], r[2], r[3], (unsigned long long)machine.frame());
            std::cout << line;
            if (log_cd_context) {
                const auto& s = machine.master().state();
                std::cout << " PC=" << hex(s.pc) << " PR=" << hex(s.pr);
                for (int i = 0; i < 8; ++i) std::cout << " r" << i << '=' << hex(s.r[i]);
            }
            std::cout << '\n';
            if (history_on_cd_reject && r[0] == 0xFF00 && history_size) print_history();
        };
        std::set<uint32_t> flow_seen;
        if (flow_frame >= 0) {
            const auto previous = machine.on_before_master_step;
            machine.on_before_master_step = [&, previous](const sh2::Sh2State& state) {
                if (previous) previous(state);
                if (machine.frame() == uint64_t(flow_frame) && flow_seen.insert(state.pc).second) std::cout << "fluxo " << hex(state.pc) << " PR=" << hex(state.pr) << '\n';
            };
        }
        std::map<int, uint64_t> interrupts_taken;
        machine.on_interrupt_taken = [&](int, int vector) { ++interrupts_taken[vector]; };
        if (bios_file.empty()) {
            machine.boot();
        } else {
            machine.boot_with_bios(frontend::read_file_bytes(bios_file));
        }
        // O contador do laço de quadros começa no frame() da máquina (0 após o boot): com --load-state, os números de quadro de
        // --frames, --shot, --pad e --save-state são os mesmos da execução contínua que gerou o estado.
        int completed = 0;
        if (!load_state_file.empty()) {
            machine.load_state(frontend::read_file_bytes(load_state_file));
            completed = static_cast<int>(machine.frame());
            std::cout << "estado carregado de " << load_state_file << " (frame() = " << machine.frame() << ")\n";
        }
        const auto take_screenshot = [&](const std::string& file) {
            video::write_png(file, frontend::capture_frame(machine, layer_mask));
            std::cout << "quadro " << machine.frame() << " gravado em " << file << '\n';
        };
        std::vector<int16_t> samples_out, chunk(8192);   // com o áudio desligado, drain_audio não devolve nada
        if (!wav_file.empty()) {
            machine.scsp().set_audio_enabled(true);
            samples_out.reserve(size_t(frames) * 1500);
        }
        std::optional<std::ofstream> frame_log_stream;
        std::ofstream* frame_log = nullptr;
        if (!frame_log_file.empty()) {
            frame_log_stream.emplace(frame_log_file);
            *frame_log_stream << "quadro,emulacao_us,video_us\n";
            frame_log = &*frame_log_stream;
        }
        const auto collect_audio = [&] {
            while (const size_t n = machine.scsp().drain_audio(chunk)) samples_out.insert(samples_out.end(), chunk.begin(), chunk.begin() + n);
        };
        try {
            if (until_pc) {
                const bool reached = machine.run_until_pc(until_pc, uint64_t(frames) * saturn::VideoTiming::kMaxCyclesPerFrame);
                std::cout << (reached ? "ALCANÇOU " : "NÃO alcançou ") << hex(until_pc) << " no quadro " << machine.frame() << '\n';
                if (history_size) print_history();
                completed = frames;
            } else {
                for (; completed < frames; ++completed) {
                    for (const auto& [frame, pressed] : pad_events)
                        if (frame == completed) machine.smpc().set_pad(static_cast<uint16_t>(~pressed));
                    for (const Poke& poke : pokes)
                        if (poke.frame == completed) machine.bus().write16(poke.address, poke.value);
                    if (completed == sample_frame) {   // PC do mestre em 64 instantes do quadro, para ver onde ele gasta o tempo
                        std::cout << "amostras de PC no quadro " << completed << ":";
                        for (int k = 0; k < 64; ++k) {
                            machine.run_cycles(machine.timing().cycles_per_frame() / 64);
                            std::cout << ' ' << hex(machine.master().state().pc);
                        }
                        std::cout << '\n';
                    } else if (frame_log) {
                        const auto t0 = std::chrono::steady_clock::now();
                        machine.run_frame();
                        const auto t1 = std::chrono::steady_clock::now();
                        frontend::capture_frame(machine, layer_mask);   // o executável desenha todo quadro
                        const auto t2 = std::chrono::steady_clock::now();
                        const auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
                        *frame_log << completed << ',' << us(t0, t1) << ',' << us(t1, t2) << '\n';
                    } else {
                        machine.run_frame();
                    }
                    collect_audio();
                    for (const auto& [frame, file] : shots)
                        if (frame == completed + 1) take_screenshot(file);
                    for (const auto& [frame, file] : save_states) {
                        if (frame != completed + 1) continue;
                        const auto state = machine.save_state();
                        frontend::write_file_atomic(file, state);
                        std::cout << "estado do quadro " << frame << " (frame() = " << machine.frame() << ", " << state.size() << " bytes, áudio coletado até aqui: "
                                  << samples_out.size() / 2 << " quadros estéreo) gravado em " << file << '\n';
                    }
                    if (completed >= vdp1_log_from && completed <= vdp1_log_to) {
                        const auto sprites = video::render_vdp1(machine.bus().ram_at(frontend::kVdp1Vram, frontend::kVdp1VramSize));
                        const auto drawn = std::count_if(sprites.data.begin(), sprites.data.end(), [](uint16_t v) { return v != 0; });
                        std::cout << "quadro " << completed << ": pixels de sprite = " << drawn << '\n';
                    }
                }
                if (history_size && !history_on_cd_reject) print_history();
            }
        } catch (const saturn::MachineError& e) {
            std::cout << "PAROU no quadro " << machine.frame() << ": " << e.what() << '\n';
            print_state(machine);
            if (history_size) print_history();
        }

        std::cout << "quadros concluídos: " << completed << " (de " << frames << ")\n";
        if (coverage) std::cout << "endereços de instrução distintos executados: " << machine.recorder().count() << '\n';
        if (machine.scsp().running()) {
            const auto& cpu68 = machine.scsp().cpu();
            std::cout << "68000: PC=" << hex(cpu68.pc) << " SR=" << hex(cpu68.sr()) << (cpu68.stopped() ? " (STOP)" : "") << '\n';
        }
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
        std::cout << "SCU: IMS=" << hex(machine.scu().read(0xA0, 4)) << " IST=" << hex(machine.scu().read(0xA4, 4)) << '\n';
        for (const auto& [vector, count] : interrupts_taken) std::cout << "interrupções atendidas, vetor " << hex(vector) << ": " << count << '\n';
        for (uint32_t address : peeks) std::cout << "[" << hex(address) << "] = " << hex(machine.bus().read32(address)) << '\n';
        if (!screenshot_file.empty()) take_screenshot(screenshot_file);
        if (!wav_file.empty()) {
            audio::write_wav_pcm16(wav_file, samples_out, int(audio::Scsp::kSampleHz), 2);
            long peak = 0;
            for (int16_t v : samples_out) peak = std::max(peak, std::labs(v));
            std::cout << "áudio: " << samples_out.size() / 2 << " quadros estéreo, pico " << peak << ", gravado em " << wav_file << '\n';
        }
        if (!dump_video_prefix.empty()) {
            const auto dump = [&](const char* suffix, uint32_t base, uint32_t size) {
                std::vector<uint8_t> bytes(size);
                for (uint32_t i = 0; i < size; ++i) bytes[i] = machine.bus().read8(base + i);
                std::ofstream(dump_video_prefix + suffix, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            };
            dump(".vdp2-vram", 0x25E00000, 0x80000);
            dump(".vdp2-cram", 0x25F00000, 0x1000);
            dump(".vdp2-regs", 0x25F80000, 0x120);
            dump(".vdp1-vram", 0x25C00000, 0x80000);
            dump(".vdp1-fb", 0x25C80000, 0x40000);
            dump(".vdp1-regs", 0x25D00000, 0x18);
            std::cout << "VDP1/VDP2 gravados com o prefixo " << dump_video_prefix << '\n';
        }
        if (!dump_ram_file.empty()) {
            const auto ram = machine.bus().ram_at(0x06000000, 0x100000);
            std::ofstream(dump_ram_file, std::ios::binary).write(reinterpret_cast<const char*>(ram.data()), ram.size());
            std::cout << "Work RAM-H gravada em " << dump_ram_file << '\n';
        }
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
