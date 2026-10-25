"""Gera a tabela estágio -> faixa de música a partir da tabela de estágios do 0.BIN (docs/analysis/01-overlays.md §2).

Hipótese [inf]: o byte baixo do campo `flags` (registros com prefixo 0xE0) é o número da faixa SD<hex>.PCM.
Uso: python3 -I tools/analysis/stage_music_table.py <pasta-do-disco-extraído>
"""
import os
import struct
import sys

BASE = 0x06004080
TABLE = 0x06047818
RECORD = 24


def main():
    root = sys.argv[1]
    data = open(os.path.join(root, "0.BIN"), "rb").read()
    be32 = lambda a: struct.unpack(">I", data[a - BASE:a - BASE + 4])[0]

    def text(a):
        off = a - BASE
        if not 0 <= off < len(data):
            return None
        s = data[off:data.index(b"\0", off)]
        return s.decode("latin1") if 0 < len(s) < 20 and s.isascii() else None

    music = {int(f[2:4], 16) for f in os.listdir(os.path.join(root, "SD"))
             if f.startswith("SD") and f.endswith(".PCM") and open(os.path.join(root, "SD", f), "rb").read(4) == b"DVI."}
    seen = {}
    addr = TABLE
    while True:
        words = [be32(addr + 4 * i) for i in range(6)]
        names = [text(w) for w in words[:3]]
        if not all(names):
            break
        addr += RECORD
        if words[4] >> 24 != 0xE0:
            continue
        seen.setdefault(names[0].removesuffix(".PRG"), (words[3], words[4] & 0xFF))

    print("| Overlay | id | faixa |\n|---|---|---|")
    for name, (rec_id, track) in sorted(seen.items(), key=lambda kv: (kv[1][1], kv[0])):
        flag = "" if track in music else " (!) sem arquivo"
        print(f"| `{name}` | `0x{rec_id:X}` | `SD{track:02X}.PCM`{flag} |")
    used = {t for _, t in seen.values()}
    print(f"\nFaixas distintas: {len(used)} de {len(music)}. Sem referência na tabela: "
          + ", ".join(f"`SD{t:02X}`" for t in sorted(music - used)))


if __name__ == "__main__":
    main()
