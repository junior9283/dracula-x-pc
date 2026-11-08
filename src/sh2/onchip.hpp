#pragma once
#include <cstdint>
#include <functional>

#include "saturn/bus.hpp"
#include "saturn/state.hpp"

namespace sh2 {

// Periféricos internos do SH7604 (0xFFFFFE00–0xFFFFFFFF): DIVU, FRT e registradores do INTC. `offset` = endereço − 0xFFFFFE00.
class OnChip : public saturn::IoDevice {
public:
    static constexpr uint32_t kBase = 0xFFFFFE00;

    uint32_t read(uint32_t offset, int bytes) override;
    void write(uint32_t offset, int bytes, uint32_t value) override;

    // Armazenamento do byte de um registrador cuja leitura é uma cópia direta dele, sem efeito colateral nem dependência do tempo
    // (TIER, FTCSR, TCR, TOCR); nullptr para os demais. Os laços ociosos da CPU vigiam esses bytes.
    const uint8_t* stable_byte(uint32_t offset) const;

    // Avança o FRT em ciclos de CPU.
    void advance(uint64_t cycles);
    // Pino de captura de entrada do FRT (MINIT/SINIT): copia o FRC para o ICR e levanta ICF.
    void signal_input_capture();

    // Estado dos periféricos após um reset do CPU (FRT, DIVU, DMAC e demais registradores zerados).
    void reset();

    // Barramento usado pelo DMAC para as transferências.
    void attach_bus(saturn::Bus& bus) { bus_ = &bus; }
    // Pino MD do modo mestre/escravo: o bit 15 (MASTER) do BCR1 lê 1 no escravo. O BIOS o consulta no reset.
    void set_slave(bool slave) { slave_ = slave; }

    // Save state: registradores do FRT, DIVU, DMAC e INTC (callbacks e o barramento ligado não entram).
    void save(saturn::StateWriter& w) const;
    void load(saturn::StateReader& r);

    // Interrupções geradas pelos periféricos (nível, vetor).
    std::function<void(int level, int vector)> irq;

private:
    uint32_t read_register(uint32_t offset) const;
    void write_register(uint32_t offset, uint32_t value);
    void divide(int64_t dividend, int32_t divisor);
    uint16_t& selected_ocr();
    uint32_t reg32(uint32_t offset) const;
    void set_reg32(uint32_t offset, uint32_t value);
    void store_bytes(uint32_t offset, int bytes, uint32_t value);
    void write_dmac(uint32_t offset, int bytes, uint32_t value);
    void run_dmac_channel(int channel);

    uint8_t bytes_[0x200] = {};  // armazenamento genérico (INTC e demais)
    // DIVU
    uint32_t dvsr_ = 0, dvdnth_ = 0, dvdntl_ = 0, dvcr_ = 0, vcrdiv_ = 0;
    // FRT
    uint16_t frc_ = 0, ocra_ = 0, ocrb_ = 0, icr_ = 0;
    uint8_t tier_ = 0, ftcsr_ = 0, tcr_ = 0, tocr_ = 0;
    uint64_t frt_residue_ = 0;
    bool slave_ = false;
    saturn::Bus* bus_ = nullptr;
};

}  // namespace sh2
