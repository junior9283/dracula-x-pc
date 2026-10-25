"""Estima o endereço-base de cada overlay .PRG (SH-2 big-endian) por votação.

Método: cada palavra de 32 bits do arquivo que parece ponteiro da Work RAM-H (0x06000000-0x060FFFFF) deve cair,
na base correta, em um prólogo de função (`sts.l pr,@-r15` = 0x4F22, possivelmente precedido por pushes 0x2FN6).
Para cada (ponteiro, prólogo) vota-se base = ponteiro - deslocamento; a base certa recebe muito mais votos.

Uso: python3 -I tools/analysis/overlay_bases.py <pasta-do-disco-extraído> [NOME.PRG ...]
"""
import collections
import pathlib
import struct
import sys

RAM_LO, RAM_HI = 0x06000000, 0x06100000
MAX_POINTERS = 400  # amostra de ponteiros por arquivo (determinística: os primeiros distintos)


def prologues(data):
    """Deslocamentos (múltiplos de 2) onde começa um prólogo de função."""
    found = []
    for off in range(0, len(data) - 3, 2):
        first = data[off:off + 2]
        if first == b"\x4f\x22" or (first[0] == 0x2F and (first[1] & 0x0F) == 0x06 and data[off + 2:off + 4] != b""):
            if off >= 2 and data[off - 2] == 0x2F and (data[off - 1] & 0x0F) == 0x06:
                continue  # não é o primeiro push da sequência
            found.append(off)
    return found


def estimate(data):
    pointers = []
    seen = set()
    for off in range(0, len(data) - 3, 4):
        word = struct.unpack(">I", data[off:off + 4])[0]
        if RAM_LO <= word < RAM_HI and word not in seen:
            seen.add(word)
            pointers.append(word)
        if len(pointers) >= MAX_POINTERS:
            break
    votes = collections.Counter(p - o for p in pointers for o in prologues(data) if (p - o) % 4 == 0 and p - o >= RAM_LO)
    if not votes:
        return None
    (base, count), = votes.most_common(1)
    second = votes.most_common(2)[1][1] if len(votes) > 1 else 0
    return base, count, second, len(pointers)


def main():
    root = pathlib.Path(sys.argv[1])
    names = sys.argv[2:] or sorted(p.name for p in root.glob("*.PRG"))
    for name in names:
        data = (root / name).read_bytes()
        result = estimate(data)
        if result is None:
            print(f"{name:14} {len(data):8}  sem candidatos")
            continue
        base, count, second, npointers = result
        print(f"{name:14} {len(data):8}  base=0x{base:08x} fim=0x{base + len(data):08x} votos={count} 2º={second} ptrs={npointers}")


if __name__ == "__main__":
    main()
