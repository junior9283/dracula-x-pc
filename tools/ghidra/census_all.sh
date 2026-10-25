#!/usr/bin/env bash
# Roda o CodeCensus em 0.BIN e em todos os overlays (cada um na sua base) e junta as linhas "CENSO" em data-local/ghidra/census.txt.
# Uso: tools/ghidra/census_all.sh   (a partir da raiz do repositório; precisa de data-local/disc e data-local/overlay-list.txt)
set -euo pipefail
G=~/opt/ghidra_12.2_PUBLIC/support/analyzeHeadless
PROJ="$PWD/data-local/ghidra"; OUT="$PROJ/census.txt"; : > "$OUT"
run() { # arquivo base
  "$G" "$PROJ" dxpc -import "$PWD/data-local/disc/$1" -processor "SuperH:BE:32:SH-2" -cspec default -loader BinaryLoader \
       -loader-baseAddr "$2" -overwrite -scriptPath "$PWD/tools/ghidra" -postScript CodeCensus.java 2>&1 | grep -E "^.*CENSO " | sed 's/.*CENSO /CENSO /; s/ (GhidraScript)//' >> "$OUT"
}
run 0.BIN 0x06004080
while read -r name base; do run "$name" "$base"; done < "$PWD/data-local/overlay-list.txt"
echo "fim: $(grep -c CENSO "$OUT") programas" >> "$OUT"
