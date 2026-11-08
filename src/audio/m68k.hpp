// Interpretador do Motorola 68000 para o driver de som do Sega Saturn.
//
// Escrito do zero a partir da documentacao publica do 68000 (sem codigo de
// emuladores GPL). Barramento de 24 bits, big-endian; toda palavra/longo passa
// por read16/write16 (um longo = duas chamadas, palavra alta primeiro).
//
// Simplificacoes documentadas:
//  - Erro de endereco (acesso de palavra/longo em endereco impar) NAO gera
//    excecao: o endereco e alinhado para baixo (bit 0 zerado) antes de chegar
//    ao barramento. O PC impar tambem e tratado assim na busca de instrucoes.
//  - A tabela de ciclos e APROXIMADA (classe da instrucao + custo do modo de
//    enderecamento); nao reproduz o tempo exato de cada variante do 68000.
//  - Excecoes sao entregues de forma "limpa": efeitos colaterais de modos
//    auto-incremento/decremento ja feitos antes de uma instrucao ilegal nao
//    sao desfeitos; nao ha frame de grupo 0 (bus/address error).
//  - Interrupcoes: apenas autovetor (24+nivel); nivel 7 nao e tratado como
//    nao mascaravel por borda (dispara sempre que pendente, como os demais
//    niveis acima da mascara, mais o proprio nivel 7 com mascara 7).
//  - RESET (instrucao) apenas consome ciclos; nao ha linha de reset externa.
#pragma once

#include <cstdint>
#include <utility>

#include "saturn/state.hpp"

namespace audio {

class M68k {
public:
    struct Bus {
        virtual uint8_t read8(uint32_t addr) = 0;
        virtual uint16_t read16(uint32_t addr) = 0;
        virtual void write8(uint32_t addr, uint8_t value) = 0;
        virtual void write16(uint32_t addr, uint16_t value) = 0;
        virtual ~Bus() = default;
    };

    // Bits do registrador de status.
    enum : uint16_t {
        kC = 0x01, kV = 0x02, kZ = 0x04, kN = 0x08, kX = 0x10,
        kIntMask = 0x0700, kSuper = 0x2000, kTrace = 0x8000,
    };

    explicit M68k(Bus& bus) : bus_(bus) {}

    // Le SSP em 0x000000 e PC em 0x000004; entra em supervisor com mascara 7.
    void reset();
    // Executa uma instrucao (ou atende uma interrupcao pendente) e devolve os ciclos.
    int step();
    // Pede interrupcao de nivel 1..7 (autovetor). Fica pendente ate a mascara permitir.
    void raise_interrupt(int level);

    // Acesso aos registradores (a[7] e sempre a pilha ativa: SSP em supervisor, USP em usuario).
    uint32_t d[8] = {};
    uint32_t a[8] = {};
    uint32_t pc = 0;

    uint16_t sr() const { return sr_; }
    void set_sr(uint16_t v);  // troca a pilha ativa se o bit S mudar
    uint32_t usp() const { return (sr_ & kSuper) ? other_sp_ : a[7]; }
    uint32_t ssp() const { return (sr_ & kSuper) ? a[7] : other_sp_; }
    void set_usp(uint32_t v) { ((sr_ & kSuper) ? other_sp_ : a[7]) = v; }
    void set_ssp(uint32_t v) { ((sr_ & kSuper) ? a[7] : other_sp_) = v; }
    bool stopped() const { return stopped_; }

    // Save state: registradores, SR, pilha inativa, interrupção pendente e STOP (o barramento não entra).
    void save(saturn::StateWriter& w) const;
    void load(saturn::StateReader& r);

private:
    // Operando efetivo ja resolvido.
    struct Ea {
        enum Kind { Dreg, Areg, Mem, Imm } kind = Mem;
        int reg = 0;
        uint32_t addr = 0;  // endereco (Mem) ou valor (Imm)
    };
    enum class Alu { Add, Sub, And, Or, Eor, Cmp };

    // Acesso ao barramento (24 bits).
    uint32_t rd(uint32_t addr, int sz);
    void wr(uint32_t addr, int sz, uint32_t v);
    uint16_t fetch16();
    uint32_t fetch32();
    void push32(uint32_t v);
    uint32_t pop32();
    uint16_t pop16();

    // Operandos.
    int32_t index_ext();
    Ea ea(int mode, int reg, int sz, unsigned allowed);
    uint32_t ea_read(const Ea& e, int sz);
    void ea_write(const Ea& e, int sz, uint32_t v);
    uint32_t& gpr(int i) { return i < 8 ? d[i] : a[i - 8]; }

    // Flags e ALU.
    bool cond(int cc) const;
    void set_nz(uint32_t r, int sz);
    uint32_t alu(Alu op, uint32_t dst, uint32_t src, int sz, bool use_x = false);
    uint32_t shift(int type, bool left, uint32_t v, int cnt, int sz);
    uint8_t bcd(bool sub, uint8_t dst, uint8_t src);
    void require_super();

    // Excecoes.
    void exception(int vec, uint32_t push_pc);

    // Decodificacao por grupo (nibble alto do opcode).
    void exec();
    void grp0();
    void grp_move();
    void grp4();
    void grp5();
    void grp6();
    void grp8();
    void grp_addsub(bool add);
    void grpB();
    void grpC();
    void grpE();
    void dyadic(Alu op);
    void bitop(int type, uint32_t bit, int mode, int reg, bool dynamic);
    void movep();
    void movem();
    void imm_to_ccr_sr(int kind);
    void bcd_op(bool sub);
    void divide(bool is_signed);
    void multiply(bool is_signed);
    void misc_no_operand();

    Bus& bus_;
    uint16_t sr_ = 0x2700;
    uint32_t other_sp_ = 0;  // pilha inativa (USP em supervisor, SSP em usuario)
    int pending_ = 0;
    bool stopped_ = false;
    uint32_t ipc_ = 0;  // endereco da instrucao em execucao
    uint16_t op_ = 0;
    int cyc_ = 0;
};

}  // namespace audio
