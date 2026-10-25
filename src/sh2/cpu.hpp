#pragma once
#include <array>
#include <cstdint>

#include "saturn/bus.hpp"

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
    bool in_delay_slot = false;
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

class Cpu;

// Registro das instruções por grupo (cada grupo vive em seu próprio .cpp e acessa o interior da CPU por amizade).
struct OpRegistrar {
    static void data(OpcodeTable& table);    // ops_data.cpp
    static void alu(OpcodeTable& table);     // ops_alu.cpp
};

class Cpu {
public:
    Cpu(saturn::Bus& bus, int id);

    void reset(uint32_t pc, uint32_t sp);
    uint32_t step();  // executa uma instrução (ou entra numa exceção) e devolve os ciclos
    Sh2State& state() { return s_; }
    int id() const { return id_; }

private:
    friend struct OpRegistrar;

    // Acessos à memória pelo barramento; endereço desalinhado → AddressError (tratado em step()).
    struct AddressError { uint32_t address; };
    uint32_t load8s(uint32_t a);
    uint32_t load16s(uint32_t a);
    uint32_t load32(uint32_t a);
    void store8(uint32_t a, uint32_t v);
    void store16(uint32_t a, uint32_t v);
    void store32(uint32_t a, uint32_t v);

    void enter_exception(int vector, uint32_t saved_pc);

    saturn::Bus& bus_;
    int id_;
    Sh2State s_;
    uint32_t op_address_ = 0;  // endereço da instrução em execução

    static const OpcodeTable& table();
};

}  // namespace sh2
