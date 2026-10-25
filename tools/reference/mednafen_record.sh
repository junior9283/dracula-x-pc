#!/usr/bin/env bash
# Grava vídeo+áudio do Mednafen (Saturn, BIOS real do usuário) como REFERÊNCIA de comportamento, sem janela na sua sessão
# e sem tocar em ~/.mednafen (home isolado; as BIOS são só linkadas). Uso local de desenvolvimento; nada é versionado.
#
# Uso: tools/reference/mednafen_record.sh <arquivo.cue> <pasta-saida> <duração-s> [instante-s ...]
#   ex.: tools/reference/mednafen_record.sh disco.cue data-local/ref 130 20 60 100
# Saída: <pasta>/audio.wav (48 kHz estéreo), <pasta>/frame_<s>s.png (352x240, um por instante pedido),
#        <pasta>/mednafen.log; o .mov bruto (cscd, ~8 MB/s) é apagado, a menos que KEEP_MOV=1.
# Variáveis: MEDNAFEN_FIRMWARE (padrão ~/.mednafen/firmware), REF_DISPLAY (padrão :95).
# Requer: Xvfb, mednafen, ffmpeg. Notas: o SDL2 prefere Wayland (abriria janela na sua sessão), por isso força-se o X11 virtual;
# o vídeo por software (-video.driver sdl) evita o OpenGL lento; o .mov guarda quadros com largura esticada (352x30): reduz-se na extração.
set -euo pipefail

cue=$(readlink -f "$1"); out=$(mkdir -p "$2" && readlink -f "$2"); duration=$3; shift 3
firmware="${MEDNAFEN_FIRMWARE:-$HOME/.mednafen/firmware}"; display="${REF_DISPLAY:-:95}"
mn_home="$out/home"; mkdir -p "$mn_home/firmware"
for f in "$firmware"/*; do ln -sf "$f" "$mn_home/firmware/"; done

unset WAYLAND_DISPLAY
export DISPLAY="$display" MEDNAFEN_HOME="$mn_home" SDL_VIDEODRIVER=x11
Xvfb "$display" -screen 0 800x600x24 >/dev/null 2>&1 & xvfb=$!
trap 'kill "$xvfb" 2>/dev/null || true' EXIT
sleep 1

timeout "$duration" mednafen -sound.driver dummy -video.driver sdl -ss.xscale 1 -ss.yscale 1 -video.fs 0 \
        -qtrecord "$out/ref.mov" -force_module ss "$cue" > "$out/mednafen.log" 2>&1 || true

ffmpeg -v error -y -i "$out/ref.mov" -vn -c:a pcm_s16le "$out/audio.wav"
for t in "$@"; do
  ffmpeg -v error -y -ss "$t" -i "$out/ref.mov" -vf scale=352:240:flags=area -frames:v 1 "$out/frame_${t}s.png"
done
[ "${KEEP_MOV:-0}" = 1 ] || rm -f "$out/ref.mov"
echo "pronto: $(ls "$out" | tr '\n' ' ')"
