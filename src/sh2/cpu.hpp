#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "saturn/bus.hpp"
#include "sh2/onchip.hpp"
#include "sh2/trace.hpp"

namespace sh2 {

// Bits do SR.
constexpr uint32_t kSrT = 1u << 0;
constexpr uint32_t kSrS = 1u << 1;
constexpr uint32_t kSrIMask = 0xFu << 4;
constexpr uint32_t kSrQ = 1u << 8;
constexpr uint32_t kSrM = 1u << 9;
constexpr uint32_t kSrWritable = kSrT | kSrS | kSrIMask | kSrQ | kSrM;  // 0x3F3

struct Sh2State {
    uint32_t r[16] = {};
    uint32_t pc = 0, pr = 0, gbr = 0, vbr = 0, mach = 0, macl = 0, sr = 0;
    bool operator==(const Sh2State&) const = default;
};

class Cpu;
// Cada instrução devolve os ciclos gastos.
using OpHandler = uint32_t (*)(Cpu&, uint16_t);

// Tabela de despacho de 65 536 entradas, preenchida por padrões de 16 caracteres: '0'/'1' fixos, qualquer outro = livre.
class OpcodeTable {
public:
    OpcodeTable();
    // Registra `handler` para todos os opcodes que casam com o padrão. Lança std::logic_error se algum já tem handler
    // (protege contra padrões sobrepostos entre grupos de instruções).
    void add(const char* pattern, OpHandler handler);
    OpHandler operator[](uint16_t op) const { return handlers_[op]; }

private:
    std::array<OpHandler, 65536> handlers_;
};

// Registro das instruções por grupo (cada grupo vive em seu próprio .cpp e acessa o interior da CPU por amizade).
struct OpRegistrar {
    static void data(OpcodeTable& table);    // ops_data.cpp
    static void alu(OpcodeTable& table);     // ops_alu.cpp
    static void branch(OpcodeTable& table);  // ops_branch.cpp
    static void mul(OpcodeTable& table);     // ops_mul.cpp
};

class Cpu {
public:
    Cpu(saturn::Bus& bus, int id);

    void reset(uint32_t pc, uint32_t sp);
    static constexpr int kNmiLevel = 16, kNmiVector = 11;   // NMI: acima de qualquer máscara; vetor 11
    // Executa uma instrução (ou entra numa exceção) e devolve os ciclos.
    uint32_t step() {
        if (inhibit_irq_ || pending_level_ || sleeping_) [[unlikely]] {
            uint32_t cycles;
            if (pre_step(cycles)) return cycles;
        }
        return execute();
    }
    // Qualquer acesso externo ao estado (HLE, ferramentas, testes) materializa antes o laço ocioso em curso.
    Sh2State& state() {
        if (idle_on_) [[unlikely]] leave_idle();
        return s_;
    }
    uint32_t pc() const { return s_.pc; }   // só vale fora de um laço ocioso (idle_active() falso)
    int id() const { return id_; }
    OnChip& onchip() { return onchip_; }
    // Liga (ou desliga, com nullptr) o gravador de endereços executados; desligado por padrão.
    void set_recorder(ExecutionRecorder* recorder) {
        leave_idle();
        recorder_ = recorder;
        if (recorder) idle_enabled_ = false;   // o gravador quer ver cada instrução
    }

    // ---- Laços ociosos (aceleração exata)
    // Um laço de espera (só lê RAM/ROM, sem escrever, e que a cada volta deixa todos os registradores como estavam) repete a mesma
    // sequência de instruções e de ciclos enquanto a memória que ele lê não muda. Dentro de Machine::run_cycles a CPU grava uma volta
    // desses laços e, enquanto a memória lida e o código não mudam (Bus::epoch + conferência dos valores), "executa" cada passo só
    // avançando a fase da volta, sem decodificar nem despachar a instrução. O estado real é reconstituído (leave_idle) a qualquer acesso.
    void set_idle_skip(bool on);                          // liga/desliga a detecção e o uso (desligado fora de run_cycles)
    void set_idle_excluded_pcs(uint32_t begin, uint32_t end) { idle_excl_begin_ = begin; idle_excl_end_ = end; }  // [begin, end): nunca num laço
    bool idle_active() const { return idle_on_; }
    bool idle_ready() { return idle_on_ && idle_check(); }   // em laço ocioso e ele continua válido
    // O laço ocioso continua valendo? Senão (interrupção atendível ou memória/registradores vigiados mudaram) ele é encerrado
    // (já materializado) e o chamador executa passos normais.
    bool idle_check() {
        if (pending_level_ && idle_irq_due()) [[unlikely]] { leave_idle(); return false; }
        if (bus_.epoch() != idle_epoch_ && !idle_revalidate()) [[unlikely]] { leave_idle(); return false; }
        return true;
    }
    // Passos de um laço ocioso já conferido por idle_check (nada pode ter mudado desde então): ciclos do próximo passo, e o consumo
    // de um crédito de ciclos (como `while (credit > 0) credit -= step()`).
    uint32_t idle_advance() {
        const uint32_t cycles = loop_.cycles[idle_phase_];
        if (++idle_phase_ == loop_.steps) idle_phase_ = 0;
        return cycles;
    }
    void idle_consume(int64_t& credit) {
        while (credit > 0) credit -= idle_advance();
    }
    void leave_idle();                  // reconstitui o estado real da fase atual e encerra o laço ocioso
    void reset_idle() { idle_on_ = false; abort_record(); cand_head_ = UINT32_MAX; cand_hits_ = 0; rejected_heads_ = {}; rejected_next_ = 0; }  // reset/load
    void abort_record() { recording_ = false; fast_areas_ = 2; }  // mudança externa (ex.: HLE) no meio da gravação

    // Pede uma interrupção (nível 1–15; 16 = NMI, que ignora SR.I). Fica pendente até ser atendida entre duas instruções.
    void raise_interrupt(int level, int vector);
    bool sleeping() const { return sleeping_; }

    // Save state: registradores, E/S interna, array de dados do cache, interrupção pendente e flags. O atalho de busca (fetch_page_) é
    // invalidado no load. O delay slot é executado dentro de step(), então não há estado de delay slot entre instruções.
    void save(saturn::StateWriter& w) const;
    void load(saturn::StateReader& r);
    std::function<void(int level, int vector)> on_interrupt_taken;

private:
    friend struct OpRegistrar;

    // Exceção de CPU lançada por acessos/instruções inválidos e tratada em step(): vetor e PC a empilhar.
    struct Exception { int vector; uint32_t saved_pc; };
    // Acessos à memória pelo barramento; endereço desalinhado → Exception{9} (erro de endereço). Os acessos a RAM/ROM e ao array de
    // dados do cache ficam inline; o resto (E/S, áreas ignoradas, E/S interna, não mapeado) vai por read_slow/write_slow.
    uint32_t read_slow(uint32_t a, int bytes);
    void write_slow(uint32_t a, int bytes, uint32_t v);
    [[noreturn]] void address_error() { throw Exception{9, op_address_}; }
    template <int Bytes> uint32_t read(uint32_t a) {
        if ((a >> 29) < fast_areas_) [[likely]] {
            const uint32_t in_page = a & saturn::Bus::kPageMask;
            if (const uint8_t* page = read_pages_[(a & 0x1FFFFFFF) >> saturn::Bus::kPageShift];
                page && in_page + Bytes <= saturn::Bus::kPageSize) [[likely]]
                return load_be<Bytes>(page + in_page);
        } else if ((a >> 29) == 6 && !recording_ && (a & 0xFFF) + Bytes <= 4096) {
            return load_be<Bytes>(&cache_data_[a & 0xFFF]);
        }
        return read_slow(a, Bytes);
    }
    template <int Bytes> void write(uint32_t a, uint32_t v) {
        if ((a >> 29) < fast_areas_ && !bus_.on_write) [[likely]] {
            const uint32_t in_page = a & saturn::Bus::kPageMask;
            if (uint8_t* page = write_pages_[(a & 0x1FFFFFFF) >> saturn::Bus::kPageShift];
                page && in_page + Bytes <= saturn::Bus::kPageSize) [[likely]] {
                store_be<Bytes>(page + in_page, v);
                bus_.bump_epoch();
                return;
            }
        } else if ((a >> 29) == 6 && !recording_ && (a & 0xFFF) + Bytes <= 4096) {
            store_be<Bytes>(&cache_data_[a & 0xFFF], v);
            return;
        }
        write_slow(a, Bytes, v);
    }
    template <int Bytes> static uint32_t load_be(const uint8_t* p) {
        if constexpr (Bytes == 1) return p[0];
        else if constexpr (Bytes == 2) return (uint32_t(p[0]) << 8) | p[1];
        else return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    template <int Bytes> static void store_be(uint8_t* p, uint32_t v) {
        if constexpr (Bytes == 1) p[0] = uint8_t(v);
        else if constexpr (Bytes == 2) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
        else { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
    }
    uint32_t load8u(uint32_t a) { return read<1>(a); }
    uint32_t load8s(uint32_t a) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(read<1>(a)))); }
    uint32_t load16s(uint32_t a) {
        if (a & 1) address_error();
        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(read<2>(a))));
    }
    uint32_t load32(uint32_t a) {
        if (a & 3) address_error();
        return read<4>(a);
    }
    void store8(uint32_t a, uint32_t v) { write<1>(a, v); }
    void store16(uint32_t a, uint32_t v) {
        if (a & 1) address_error();
        write<2>(a, v);
    }
    void store32(uint32_t a, uint32_t v) {
        if (a & 3) address_error();
        write<4>(a, v);
    }

    // Busca de instrução (endereço par): lê direto da página de RAM/ROM em cache; o resto vai por read_slow().
    uint16_t fetch16(uint32_t address) {
        if ((address >> saturn::Bus::kPageShift) != fetch_tag_ || bus_.map_generation() != fetch_generation_) [[unlikely]]
            refresh_fetch_page(address);
        if (!fetch_page_) [[unlikely]] return static_cast<uint16_t>(read_slow(address, 2));
        const uint32_t offset = address & saturn::Bus::kPageMask;
        return static_cast<uint16_t>((fetch_page_[offset] << 8) | fetch_page_[offset + 1]);
    }
    void refresh_fetch_page(uint32_t address);
    // Executa a instrução em s_.pc (sem a checagem de interrupção/sono); exceções viram entrada em vetor.
    uint32_t execute() {
        op_address_ = s_.pc;
        try {
            if (s_.pc & 1) [[unlikely]] throw Exception{9, s_.pc};
            const uint16_t op = fetch16(s_.pc);
            s_.pc += 2;
            if (OpHandler handler = table_[op]) [[likely]] {
                if (recorder_) [[unlikely]] recorder_->record(op_address_);
                const uint32_t cycles = handler(*this, op);
                if (recording_) [[unlikely]] {
                    record_step(cycles);
                } else if (s_.pc <= op_address_ && idle_enabled_) {   // desvio para trás: candidato a laço de espera
                    if (s_.pc != cand_head_) { cand_head_ = s_.pc; cand_hits_ = 0; }
                    else if (++cand_hits_ == kIdleCandidateHits) [[unlikely]] begin_record();
                    if (++sample_count_ >= kIdleSampleEvery) [[unlikely]] begin_record();   // laços longos, com vários desvios para trás
                }
                return cycles;
            }
            throw Exception{4, op_address_};
        } catch (const Exception& e) {
            if (recording_) abort_record();
            enter_exception(e.vector, e.saved_pc);
        }
        return 8;
    }
    // Checagens antes da instrução quando há interrupção pendente, LDC SR recente ou CPU dormindo; true = o passo já foi consumido.
    bool pre_step(uint32_t& cycles);

    void enter_exception(int vector, uint32_t saved_pc);
    // Executa a instrução do delay slot (em op_address_+2) e só então salta para `target`. Desvios no slot → exceção 6.
    uint32_t delay_slot(uint32_t target);
    bool take_interrupt_if_due();

    // ---- Laços ociosos (ver set_idle_skip)
    struct IdleLoop {
        static constexpr int kMaxSteps = 256, kMaxWatches = 128;
        struct Watch { const uint8_t* word; uint32_t mask, value; };   // palavra de 4 bytes alinhada; confere (palavra & mask) == value
        int steps = 0;
        uint32_t cycles[kMaxSteps];          // ciclos do passo executado a partir da fase p
        uint32_t op_address[kMaxSteps];      // op_address_ na fase p
        Sh2State state[kMaxSteps];           // estado na fase p (p passos da volta já executados)
        Watch watches[kMaxWatches];
        int watch_count = 0;
        struct ByteWatch { const uint8_t* byte; uint8_t value; };   // registrador de E/S interna (byte)
        ByteWatch byte_watches[kMaxWatches];
        int byte_watch_count = 0;
    };
    static constexpr uint32_t kIdleSampleEvery = 2048;  // ... ou uma tentativa a cada tantos desvios para trás, em qualquer ponto
    static constexpr uint32_t kIdleCandidateHits = 8;   // voltas seguidas ao mesmo destino antes de tentar gravar o laço
    void begin_record();
    void record_step(uint32_t cycles);
    void finish_record_ok();
    void fail_record();
    bool add_watch(uint32_t address, int bytes);
    bool watch_read(uint32_t address, int bytes);   // vigia uma leitura feita durante a gravação; false = leitura com efeito/dependência do tempo
    bool idle_irq_due() const;
    bool idle_revalidate();

    saturn::Bus& bus_;
    const uint8_t* const* read_pages_ = bus_.fast_read_table();
    uint8_t* const* write_pages_ = bus_.fast_write_table();
    uint32_t fast_areas_ = 2;   // áreas (endereço >> 29) com caminho rápido por tabela: 2 normalmente, 0 enquanto grava um laço
    int id_;
    Sh2State s_;
    OnChip onchip_;
    ExecutionRecorder* recorder_ = nullptr;
    std::array<uint8_t, 4096> cache_data_{};   // array de dados do cache (0xC0000000), usado como RAM rápida
    uint32_t op_address_ = 0;  // endereço da instrução em execução
    void invalidate_fetch_cache() { fetch_page_ = nullptr; fetch_tag_ = UINT32_MAX; fetch_generation_ = UINT32_MAX; }
    const uint8_t* fetch_page_ = nullptr;          // página da última busca (nullptr = passar pelo barramento)
    uint32_t fetch_tag_ = UINT32_MAX;              // endereço >> Bus::kPageShift dessa página
    uint32_t fetch_generation_ = UINT32_MAX;       // Bus::map_generation() quando ela foi resolvida

    static const OpcodeTable& table();
    const OpcodeTable& table_ = table();  // guardada: evita a guarda do static local a cada instrução

    IdleLoop loop_;
    bool idle_enabled_ = false, idle_on_ = false, recording_ = false;
    int idle_phase_ = 0;
    uint32_t loop_gen_ = 0;   // Bus::map_generation() quando o laço foi gravado
    uint64_t idle_epoch_ = 0;
    uint32_t idle_excl_begin_ = 0, idle_excl_end_ = 0;
    uint32_t cand_head_ = UINT32_MAX;     // destino do último desvio para trás e quantas vezes seguidas se repetiu
    uint32_t cand_hits_ = 0;
    uint32_t sample_count_ = 0;           // desvios para trás desde a última tentativa de gravação
    std::array<uint32_t, 8> rejected_heads_ = {};     // destinos que já falharam como laço ocioso (descartados sem regravar)
    uint32_t rejected_next_ = 0;
    int record_steps_ = 0;
    Sh2State record_state_[IdleLoop::kMaxSteps];   // estado depois de cada passo gravado
    uint32_t record_op_[IdleLoop::kMaxSteps];
    Sh2State record_start_;
    uint32_t record_start_op_ = 0;

    int pending_level_ = 0;      // 0 = nenhuma
    int pending_vector_ = 0;
    bool sleeping_ = false;
    bool inhibit_irq_ = false;   // LDC/LDC.L ...,SR bloqueia a interrupção por uma instrução
};

}  // namespace sh2
