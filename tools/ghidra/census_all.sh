#!/usr/bin/env bash
# Roda o CodeCensus em 0.BIN e em todos os overlays (cada um na sua base) e junta as linhas "CENSO" em data-local/ghidra/census.txt.
# Execute a partir da raiz do repositório. Precisa de data-local/disc (dxpc-extract) e de data-local/overlay-list.txt
# (dxpc-overlays <cue> > data-local/overlays.txt; python3 -I tools/analysis/overlay_list.py data-local/overlays.txt > data-local/overlay-list.txt).
# Variável opcional: GHIDRA_HOME (padrão ~/opt/ghidra_12.2_PUBLIC).
set -euo pipefail
GHIDRA_HOME="${GHIDRA_HOME:-$HOME/opt/ghidra_12.2_PUBLIC}"
G="$GHIDRA_HOME/support/analyzeHeadless"
PROJ="$PWD/data-local/ghidra"; OUT="$PROJ/census.txt"; : > "$OUT"
failed=0
run() { # arquivo base
  local line
  line=$("$G" "$PROJ" dxpc -import "$PWD/data-local/disc/$1" -processor "SuperH:BE:32:SH-2" -cspec default -loader BinaryLoader \
       -loader-baseAddr "$2" -overwrite -scriptPath "$PWD/tools/ghidra" -postScript CodeCensus.java 2>&1 | grep -E "CENSO " || true)
  if [ -z "$line" ]; then echo "FALHA $1" >> "$OUT"; failed=$((failed + 1)); return; fi
  echo "$line" | sed 's/.*CENSO /CENSO /; s/ (GhidraScript)//' >> "$OUT"
}
run 0.BIN 0x06004080
while read -r name base; do run "$name" "$base"; done < "$PWD/data-local/overlay-list.txt"
echo "fim: $(grep -c '^CENSO ' "$OUT") programas, $failed falha(s)" >> "$OUT"
