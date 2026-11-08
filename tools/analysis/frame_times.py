#!/usr/bin/env python3
"""Resume o CSV do `dxpc-run --frame-log` (quadro, emulacao_us, video_us): média, p95, p99, máximo e quadros acima de 16,7 ms.
Uso: python3 tools/analysis/frame_times.py arquivo.csv [quadro_inicial quadro_final]
"""
import csv
import sys

rows = [(int(a), int(b), int(c)) for a, b, c in list(csv.reader(open(sys.argv[1])))[1:]]
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 62
sel = [(q, b, c) for q, b, c in rows if lo <= q < hi]
total = sorted(b + c for _, b, c in sel)
n = len(total)
print(f"quadros {n}  média {sum(total) / n / 1000:.2f} ms  p95 {total[int(n * .95)] / 1000:.2f}  p99 {total[int(n * .99)] / 1000:.2f}  "
      f"máx {total[-1] / 1000:.2f}  acima de 16,7 ms: {sum(1 for t in total if t > 16700)}")
print(f"emulação média {sum(b for _, b, _ in sel) / n / 1000:.2f} ms · vídeo médio {sum(c for _, _, c in sel) / n / 1000:.2f} ms")
