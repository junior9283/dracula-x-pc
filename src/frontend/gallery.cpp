#include "frontend/gallery.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace frontend {
namespace {

constexpr int kRate = audio::kDviSampleRate;
constexpr size_t kSector = saturnfs::kUserSectorSize;

// Faixa -> arquivos de fase que a usam. FONTE: docs/analysis/03-tabela-musica.md (tabela de estágios do 0.BIN em 0x06047818,
// campo flags 0xE0xx, byte baixo = número da faixa em hexadecimal) [inferido]; 24 das 49 faixas. As 25 restantes não passam por
// essa tabela (chefes, título, créditos...) e ficam sem local até o registro em tempo de execução.
// Cenário ilustrativo ('theme'): escolha ARTÍSTICA (nada no disco diz como é cada fase). Os estágios normais alternam entre os
// temas; os "invertidos" (RSTAGE*, o castelo invertido, segundo docs/analysis/01-overlays.md) usam o tema próprio.
struct PlaceRow {
    int track;
    const char* label;
    const char* overlays;
    Theme theme;
};
constexpr PlaceRow kPlaces[] = {
    {0x01, "Estágio 06", "STAGE_06", Theme::kCastle},
    {0x02, "Estágio 10", "STAGE_10", Theme::kCaverns},
    {0x03, "Estágio 12", "STAGE_12, RSTAGE12", Theme::kCastle},
    {0x04, "Estágio 02", "STAGE_02", Theme::kLibrary},
    {0x05, "Estágio 13", "STAGE_13", Theme::kGraveyard},
    {0x06, "Estágio 05", "STAGE_05", Theme::kClockTower},
    {0x07, "Estágio 09", "STAGE_09", Theme::kChapel},
    {0x08, "Estágio 07", "STAGE_07, RSTAGE07", Theme::kGraveyard},
    {0x09, "Estágio 03", "STAGE_03", Theme::kChapel},
    {0x0A, "Estágio 01", "STAGE_01, STAGE_0X", Theme::kCastle},
    {0x0B, "Estágio 08", "STAGE_08", Theme::kLibrary},
    {0x0C, "Estágio 04", "STAGE_04", Theme::kCaverns},
    {0x0D, "Estágio 11", "STAGE_11", Theme::kClockTower},
    {0x0E, "Estágio 14", "STAGE_14, RSTAGE14", Theme::kChapel},
    {0x10, "Invertidos", "RSTAGE05, 06, 09", Theme::kInverted},
    {0x11, "Invertido 11", "RSTAGE11", Theme::kInverted},
    {0x12, "Invertido 13", "RSTAGE13", Theme::kInverted},
    {0x17, "Invertidos", "RSTAGE01, 02, 03, 04, 08, 10", Theme::kInverted},
    {0x19, "Estágio 00", "STAGE_00", Theme::kGraveyard},
    {0x25, "Estágio 15", "STAGE_15", Theme::kClockTower},
    {0x26, "Estágio 16", "STAGE_16", Theme::kCastle},
    {0x29, "Invertido 15", "RSTAGE15", Theme::kInverted},
    {0x2B, "Chefe 15", "BOSS_15", Theme::kGeneric},
    {0x2C, "Invertido 16", "RSTAGE16", Theme::kInverted},
};

// "SD0A.PCM" -> 10; 0 se o nome não for desse formato.
int track_number(const std::string& file) {
    if (file.size() < 7 || file.size() > 8 || (file[0] | 0x20) != 's' || (file[1] | 0x20) != 'd') return 0;
    const size_t dot = file.find('.');
    if (dot == std::string::npos || dot < 3 || dot > 4) return 0;
    std::string ext = file.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });
    if (ext != "PCM") return 0;
    int value = 0;
    for (size_t i = 2; i < dot; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(file[i]))) return 0;
        value = value * 16 + (std::isdigit(static_cast<unsigned char>(file[i])) ? file[i] - '0' : (file[i] | 0x20) - 'a' + 10);
    }
    return value;
}

}  // namespace

Place place_for_track(int number) {
    for (const auto& row : kPlaces)
        if (row.track == number) return {row.label, row.overlays, row.theme};
    return {};
}

std::vector<TrackInfo> build_track_catalog(const saturnfs::SectorReader& source) {
    std::vector<TrackInfo> tracks;
    const auto root = saturnfs::root_directory(source);
    const auto root_entries = saturnfs::list_directory(source, root.lba, root.size);
    const auto sd = std::find_if(root_entries.begin(), root_entries.end(), [](const saturnfs::DirEntry& e) { return e.is_directory && e.name == "SD"; });
    if (sd == root_entries.end()) return tracks;
    for (const auto& item : saturnfs::list_directory(source, sd->lba, sd->size)) {   // só "SD/", sem varrer o disco todo
        if (item.is_directory || item.size < 0x800) continue;
        const int number = track_number(item.name);
        const saturnfs::FileEntry entry{"SD/" + item.name, item.lba, item.size};
        if (number == 0 || !saturnfs::is_in_data_track(source, entry)) continue;
        const auto head = source.read_user_sector(entry.lba);   // erro de leitura do disco propaga (load() registra em error_)
        try {
            TrackInfo t;
            t.header = audio::parse_dvi_header(head, entry.size);
            t.number = number;
            t.file = item.name;
            t.entry = entry;
            t.seconds = double(t.header.body_size) / kRate;
            if (t.header.loop_start_frames && *t.header.loop_start_frames < t.header.body_size)
                t.loop_seconds = double(*t.header.loop_start_frames) / kRate;
            t.place = place_for_track(number);
            tracks.push_back(std::move(t));
        } catch (const std::runtime_error&) {
            // cabeçalho que não é DVI (ou corrompido): fora da galeria
        }
    }
    std::sort(tracks.begin(), tracks.end(), [](const TrackInfo& a, const TrackInfo& b) { return a.number < b.number; });
    return tracks;
}

// ---- TrackStream ----

TrackStream::TrackStream(const saturnfs::SectorReader& source, const TrackInfo& track, bool loop)
    : source_(source), track_(track), state_{track.header.channel[0], track.header.channel[1]} {
    total_groups_ = track.header.body_size / audio::kDviGroupBytes;
    loop_group_ = track.header.loop_start_frames ? *track.header.loop_start_frames / audio::kFramesPerGroup : 0;
    loops_ = loop && track.header.loop_start_frames && loop_group_ < total_groups_;
}

size_t TrackStream::read(std::span<int16_t> out) {
    constexpr size_t kGroupSamples = audio::kFramesPerGroup * 2;
    size_t written = 0;
    while (written + kGroupSamples <= out.size()) {
        if (group_ >= total_groups_) {
            if (!loops_) break;
            group_ = loop_group_;
            std::copy(std::begin(loop_state_), std::end(loop_state_), state_);
        }
        if (loops_ && !have_loop_state_ && group_ == loop_group_) {
            std::copy(std::begin(state_), std::end(state_), loop_state_);
            have_loop_state_ = true;
        }
        uint64_t end = total_groups_;   // não atravessa o ponto de laço antes de guardar o estado ali
        if (loops_ && !have_loop_state_ && group_ < loop_group_) end = loop_group_;

        const uint64_t offset = track_.header.header_size + group_ * audio::kDviGroupBytes;
        const int64_t index = int64_t(track_.entry.lba + offset / kSector);
        if (index != sector_index_) {
            sector_ = source_.read_user_sector(uint32_t(index));
            sector_index_ = index;
        }
        const size_t in_sector = size_t(offset % kSector);
        const uint64_t groups = std::min<uint64_t>({(kSector - in_sector) / audio::kDviGroupBytes, end - group_,
                                                    (out.size() - written) / kGroupSamples});
        audio::decode_dvi_groups(std::span<const uint8_t>(sector_.data() + in_sector, groups * audio::kDviGroupBytes), state_,
                                 out.data() + written);
        group_ += groups;
        written += groups * kGroupSamples;
        decoded_frames_ += groups * audio::kFramesPerGroup;
    }
    return written;
}

double track_position_seconds(const TrackInfo& track, uint64_t frames) {
    const uint64_t total = track.header.body_size;
    if (frames < total || !track.loop_seconds) return double(std::min(frames, total)) / kRate;
    const uint64_t loop_start = *track.header.loop_start_frames / audio::kFramesPerGroup * audio::kFramesPerGroup;
    const uint64_t span = std::max<uint64_t>(total - loop_start, 1);
    return double(loop_start + (frames - total) % span) / kRate;
}

// ---- Gallery ----

void Gallery::load() {
    if (loaded_) return;
    try {
        tracks_ = build_track_catalog(source_);
    } catch (const std::exception& e) {
        error_ = std::string("Erro ao ler o disco: ") + e.what();
        return;
    }
    error_.clear();
    loaded_ = true;
    selected_ = scroll_ = 0;
}

void Gallery::keep_visible() {
    const int n = int(tracks_.size());
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + kGalleryRows) scroll_ = selected_ - kGalleryRows + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, n - kGalleryRows));
}

void Gallery::select(int index) {
    if (tracks_.empty()) return;
    const int clamped = std::clamp(index, 0, int(tracks_.size()) - 1);
    if (clamped != selected_) scene_start_ = time_;
    selected_ = clamped;
    keep_visible();
}

void Gallery::move(int step) { select(selected_ + step); }

void Gallery::step_track(int direction) {
    if (tracks_.empty()) return;
    const int n = int(tracks_.size());
    select(((selected_ + direction) % n + n) % n);
    if (playing()) play(selected_);
}

void Gallery::toggle_play() {
    if (tracks_.empty()) return;
    if (playing_index() == selected_) stop();
    else play(selected_);
}

void Gallery::play(int index) {
    if (index < 0 || index >= int(tracks_.size())) return;
    stream_ = std::make_unique<TrackStream>(source_, tracks_[index], true);
    queued_frames_ = 0;
    error_.clear();
    flush_ = true;
}

int Gallery::playing_index() const {
    if (!stream_) return -1;
    const int number = stream_->track().number;
    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [number](const TrackInfo& t) { return t.number == number; });
    return int(it - tracks_.begin());
}

void Gallery::stop() {
    if (stream_) flush_ = true;
    stream_.reset();
    queued_frames_ = 0;
    target_ = {};
}

void Gallery::pump(int queued_bytes, const std::function<void(std::span<const int16_t>)>& push) {
    if (!stream_) return;
    queued_frames_ = queued_bytes / 4;
    try {
        std::array<int16_t, 2 * kChunkFrames> chunk;
        size_t last = 0;   // amostras do último bloco lido: só ele alimenta o espectro
        for (int guard = 0; queued_frames_ < kTargetQueueFrames && guard < 8; ++guard) {
            if (stream_->finished()) {   // faixa sem laço acabou: para quando a fila esvaziar
                if (queued_frames_ == 0) stop();
                return;
            }
            const size_t n = stream_->read(chunk);
            if (n == 0) continue;
            push(std::span<const int16_t>(chunk.data(), n));
            queued_frames_ += int(n / 2);
            last = n;
        }
        if (last) target_ = compute_spectrum(std::span<const int16_t>(chunk.data(), last), kSampleRate);
    } catch (const std::exception& e) {
        error_ = std::string("Erro ao ler a faixa: ") + e.what();
        stop();
    }
}

void Gallery::advance(double dt) {
    time_ += dt;
    const float attack = float(std::min(1.0, dt * 30)), fall = float(dt * 1.8);
    for (int i = 0; i < kSpectrumBands; ++i) {
        float& s = spectrum_[i];
        s = target_[i] > s ? s + (target_[i] - s) * attack : std::max(target_[i], s - fall);
    }
}

float Gallery::energy() const {
    float sum = 0;
    for (int i = 0; i < kSpectrumBands / 2; ++i) sum += spectrum_[i];
    return std::clamp(sum / float(kSpectrumBands / 2) * 1.6f, 0.0f, 1.0f);
}

double Gallery::position_seconds() const {
    if (!stream_) return 0;
    const uint64_t decoded = stream_->decoded_frames(), queued = uint64_t(queued_frames_);
    return track_position_seconds(stream_->track(), decoded > queued ? decoded - queued : 0);
}

}  // namespace frontend
