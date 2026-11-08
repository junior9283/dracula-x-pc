// Galeria da trilha sonora: catálogo das faixas SD/SDxx.PCM (DVI/IMA ADPCM) lidas do pacote local, decodificação em streaming com
// laço e o estado da página (seleção, reprodução, espectro, tempo da animação). Sem SDL: o aplicativo só entrega os bytes que o
// dispositivo de áudio ainda tem na fila e recebe de volta o PCM a enfileirar.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio/dvi.hpp"
#include "frontend/scenes.hpp"
#include "saturnfs/iso9660.hpp"
#include "saturnfs/sector_reader.hpp"

namespace frontend {

// Local de uso da faixa no jogo. 'overlays' vem de docs/analysis/03-tabela-musica.md (tabela de estágios do 0.BIN, inferida);
// o nome do local no jogo NÃO é conhecido, então 'label' é só o nome do arquivo de fase. O cenário ('theme') é uma escolha
// artística ilustrativa, não o local real.
struct Place {
    std::string label;      // "Estágio 06", "Invertido 12"...; vazio = faixa sem local conhecido
    std::string overlays;   // "STAGE_06"; vários separados por vírgula
    Theme theme = Theme::kGeneric;
    bool known() const { return !label.empty(); }
};

Place place_for_track(int number);   // número 1..49 (o hexadecimal do nome do arquivo, em decimal)

struct TrackInfo {
    int number = 0;                       // 1..49
    std::string file;                     // "SD0A.PCM"
    saturnfs::FileEntry entry;
    audio::DviHeader header;
    double seconds = 0;
    std::optional<double> loop_seconds;   // início do laço; vazio = sem laço
    Place place;
};

// Faixas SD/SD*.PCM com cabeçalho DVI válido, em ordem de número. Arquivos inválidos são ignorados.
std::vector<TrackInfo> build_track_catalog(const saturnfs::SectorReader& source);

// Lê uma faixa aos poucos (um setor por vez). Com laço, ao chegar ao fim volta ao ponto de laço do cabeçalho (+0x0C) com o estado
// do decodificador que a primeira passada tinha ali, então a emenda não faz estalo. Sem laço, termina.
class TrackStream {
public:
    TrackStream(const saturnfs::SectorReader& source, const TrackInfo& track, bool loop = true);

    // Preenche até out.size() amostras (L,R intercaladas, múltiplo de 16 aproveitado); devolve quantas gravou. 0 só no fim sem laço.
    size_t read(std::span<int16_t> out);
    bool finished() const { return !loops_ && group_ >= total_groups_; }
    const TrackInfo& track() const { return track_; }
    uint64_t decoded_frames() const { return decoded_frames_; }   // total entregue desde o início (não zera no laço)

private:
    const saturnfs::SectorReader& source_;
    TrackInfo track_;
    bool loops_;
    uint64_t total_groups_ = 0, loop_group_ = 0, group_ = 0, decoded_frames_ = 0;
    audio::DviChannelState state_[2], loop_state_[2];
    bool have_loop_state_ = false;
    std::array<uint8_t, saturnfs::kUserSectorSize> sector_{};
    int64_t sector_index_ = -1;
};

// Posição (s) dentro da faixa depois de 'frames' quadros decodificados, com o laço aplicado.
double track_position_seconds(const TrackInfo& track, uint64_t frames);

class Gallery {
public:
    // 'source' precisa viver mais que a galeria.
    explicit Gallery(const saturnfs::SectorReader& source) : source_(source) {}

    void load();   // monta o catálogo uma vez; se o disco não tiver ISO9660 deixa a mensagem em error() e tenta de novo na próxima
    bool loaded() const { return loaded_; }
    const std::vector<TrackInfo>& tracks() const { return tracks_; }

    // Lista: seleção e rolagem (linhas visíveis fixas em kGalleryRows).
    static constexpr int kGalleryRows = 9;
    int selected() const { return selected_; }
    int scroll() const { return scroll_; }
    void select(int index);
    void move(int step);              // sobe/desce com limite nas pontas
    void step_track(int direction);   // esquerda/direita: troca de faixa; se estava tocando, toca a nova
    void toggle_play();               // toca a faixa selecionada, ou para
    void play(int index);
    void stop();
    bool playing() const { return bool(stream_); }
    int playing_index() const;   // índice da faixa tocando, ou -1
    // Verdadeiro uma vez depois de play()/stop(): o aplicativo deve descartar o que sobrou na fila do dispositivo.
    bool take_flush() { return std::exchange(flush_, false); }

    // Mantém a fila do dispositivo com ~0,25 s: decodifica e chama push(pcm) até lá. 'queued_bytes' = bytes ainda na fila.
    void pump(int queued_bytes, const std::function<void(std::span<const int16_t>)>& push);
    // Avança a animação e faz as barras do espectro caírem.
    void advance(double dt);

    double time() const { return time_; }
    double scene_age() const { return time_ - scene_start_; }   // segundos desde a última troca de faixa selecionada (para o fade)
    const Spectrum& spectrum() const { return spectrum_; }
    float energy() const;
    double position_seconds() const;      // posição de reprodução atual (descontando o que está na fila)
    const std::string& error() const { return error_; }
    const TrackInfo* current() const { return tracks_.empty() ? nullptr : &tracks_[selected_]; }

    static constexpr int kSampleRate = audio::kDviSampleRate;
    static constexpr int kTargetQueueFrames = kSampleRate / 4;
    static constexpr int kChunkFrames = 2048;

private:
    void keep_visible();

    const saturnfs::SectorReader& source_;
    std::vector<TrackInfo> tracks_;
    bool loaded_ = false, flush_ = false;
    int selected_ = 0, scroll_ = 0;
    std::unique_ptr<TrackStream> stream_;
    int queued_frames_ = 0;
    Spectrum target_{}, spectrum_{};
    double time_ = 0, scene_start_ = -100;
    std::string error_;
};

// Desenha a página inteira (cenário, lista, informações, espectro e rodapé) em "overlay", opaca.
void draw_gallery(const Gallery& gallery, video::Frame& overlay);
// Faixa (índice do catálogo) sob o ponto do overlay, ou -1.
int gallery_row_at(const Gallery& gallery, int x, int y);

}  // namespace frontend
