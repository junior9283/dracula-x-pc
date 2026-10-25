#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace audio {

// Taxa de amostragem das faixas DVI. INFERÊNCIA: o cabeçalho não a informa (docs/analysis/02-musica-dvi.md).
constexpr int kDviSampleRate = 44100;

struct DviChannelState {
    int16_t predictor;
    int index;  // índice na tabela de passos IMA, 0..88
};

struct DviHeader {
    uint32_t header_size;
    uint32_t body_size;
    uint32_t loop_start_frames;  // INFERÊNCIA: início do trecho em laço, em quadros estéreo
    DviChannelState channel[2];
};

// Lança std::runtime_error se o arquivo tem menos de 0x800 bytes, magic != "DVI.", header_size + body_size != tamanho
// do arquivo, ou índice de passo > 88.
DviHeader parse_dvi_header(std::span<const uint8_t> file);

// IMA ADPCM estéreo (grupos de 8 bytes = 4 bytes L + 4 bytes R, nibble alto primeiro). Devolve PCM 16 bits
// intercalado L,R. Bytes finais que não completam um grupo de 8 são ignorados.
std::vector<int16_t> decode_dvi(std::span<const uint8_t> body, const DviHeader& header);

}  // namespace audio
