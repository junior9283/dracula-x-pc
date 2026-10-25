"""Converte a saída de `dxpc-overlays <cue>` na lista "NOME.PRG 0xBASE" usada por tools/ghidra/census_all.sh.

Uso: dxpc-overlays <cue> > data-local/overlays.txt && python3 -I tools/analysis/overlay_list.py data-local/overlays.txt > data-local/overlay-list.txt
"""
import re
import sys

for line in open(sys.argv[1]):
    m = re.match(r"(\S+\.PRG)\s+0x([0-9a-f]{8})-", line)
    if m:
        print(f"{m.group(1)} 0x{m.group(2)}")
