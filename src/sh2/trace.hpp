#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace sh2 {

// Conjunto dos endereços de instrução já executados (granularidade de 2 bytes; o alias de cache-through 0x2... é o mesmo
// endereço de 0x0...). Alimenta a recompilação AOT guiada por traço. Bitmap em blocos de 1 MB alocados sob demanda.
class ExecutionRecorder {
public:
    ExecutionRecorder();

    void record(uint32_t pc);
    bool executed(uint32_t pc) const;
    size_t count() const { return count_; }

    // Formato: "DXTR", versão (u32), nº de faixas (u32), depois pares (início, nº de instruções consecutivas) em u32 little-endian.
    void save(const std::filesystem::path& file) const;
    static ExecutionRecorder load(const std::filesystem::path& file);

private:
    static constexpr uint32_t kBlockShift = 20;                          // 1 MB por bloco
    static constexpr uint32_t kBlocks = 0x20000000u >> kBlockShift;      // espaço físico de 29 bits
    using Block = std::array<uint64_t, (1u << kBlockShift) / 2 / 64>;    // 1 bit por palavra de 16 bits

    std::vector<std::unique_ptr<Block>> blocks_;
    size_t count_ = 0;
};

}  // namespace sh2
