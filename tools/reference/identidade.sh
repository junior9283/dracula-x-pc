#!/usr/bin/env bash
# Gera as saídas de referência (imagem, áudio e ESTADO COMPLETO da máquina) para provar que uma mudança não alterou o comportamento.
# Uso: tools/reference/identidade.sh <dxpc-run> <pasta-de-saida>
# Rode uma vez com o binário de referência e outra com o novo; depois compare com `cmp` cada arquivo (todos têm de ser idênticos).
# Precisa de data-local/boot-font.bin e (para a parte do BIOS real) de data-local/bios-local.bin; o disco fica em ../CASTLEVANIA SON/.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
run="$1"; out="$2"; mkdir -p "$out"
cue="$(ls "$root"/../"CASTLEVANIA SON"/*/*.cue | head -1)"
"$run" "$cue" --lenient-bios --boot-font "$root/data-local/boot-font.bin" --frames 9600 $(cat "$root/tools/reference/pad-gameplay.txt") \
    --shot 4000:"$out/hle4000.png" --shot 9600:"$out/hle9600.png" --save-state 9600:"$out/hle9600.dxs" --wav "$out/hle.wav" \
    --frame-log "$out/frames.csv" > /dev/null 2>&1 &
if [ -f "$root/data-local/bios-local.bin" ]; then
    "$run" "$cue" --bios "$root/data-local/bios-local.bin" --frames 2400 --shot 2400:"$out/bios2400.png" \
        --save-state 2400:"$out/bios2400.dxs" --wav "$out/bios.wav" > /dev/null 2>&1 &
fi
wait
sha1sum "$out"/*.png "$out"/*.wav "$out"/*.dxs
