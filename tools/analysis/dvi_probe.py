"""Sonda de decodificação para os fluxos DVI: testa layouts e variantes de ADPCM e mede a energia subsônica.

Critério: áudio decodificado corretamente tem pouca energia < 20 Hz (um decodificador errado vira passeio aleatório,
com >80% da energia nessa faixa). Uso: python3 -I tools/analysis/dvi_probe.py <pasta SD> [SD03.PCM ...]
"""
import struct
import sys

import numpy as np

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
        1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
        8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]
RATE = 44100


def ima(nibs, pred, idx):
    out = np.empty(len(nibs))
    for k, n in enumerate(nibs):
        s = STEP[idx]
        d = s >> 3
        if n & 1: d += s >> 2
        if n & 2: d += s >> 1
        if n & 4: d += s
        if n & 8: d = -d
        pred = max(-32768, min(32767, pred + d))
        idx = max(0, min(88, idx + IDX[n & 7]))
        out[k] = pred
    return out


def yamaha_b(nibs, pred, _idx):
    """ADPCM-B (YM2608): delta adaptativo, 4 bits sinal+magnitude."""
    adj = [57, 57, 57, 57, 77, 102, 128, 153]
    delta = 127
    out = np.empty(len(nibs))
    for k, n in enumerate(nibs):
        mag = n & 7
        step = ((2 * mag + 1) * delta) >> 3
        pred = max(-32768, min(32767, pred + (-step if n & 8 else step)))
        delta = max(127, min(24576, (delta * adj[mag]) >> 6))
        out[k] = pred
    return out


def subsonic(x):
    x = x[-65536:]
    sp = np.abs(np.fft.rfft((x - x.mean()) * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / RATE)
    return float(sp[f < 20].sum() / sp.sum())


def layouts(body, group):
    """Gera (nome, [nibbles do canal A, nibbles do canal B ou None]) para cada intercalação."""
    b = list(body)
    lh = lambda v: [v & 15, v >> 4]
    hl = lambda v: [v >> 4, v & 15]
    yield "mono lo→hi", [[n for v in b for n in lh(v)], None]
    yield "mono hi→lo", [[n for v in b for n in hl(v)], None]
    yield "nibble L=lo R=hi", [[v & 15 for v in b], [v >> 4 for v in b]]
    yield "nibble L=hi R=lo", [[v >> 4 for v in b], [v & 15 for v in b]]
    for g in group:
        for order, f in (("lo→hi", lh), ("hi→lo", hl)):
            L, R = [], []
            for i in range(0, len(b) - 2 * g + 1, 2 * g):
                L += [n for v in b[i:i + g] for n in f(v)]
                R += [n for v in b[i + g:i + 2 * g] for n in f(v)]
            yield f"grupos de {g} byte(s) {order}", [L, R]


def main():
    root = sys.argv[1]
    names = sys.argv[2:] or ["SD03.PCM", "SD04.PCM"]
    for name in names:
        with open(f"{root}/{name}", "rb") as fh:
            h = fh.read(0x30)
            fh.seek(0x800)
            body = fh.read(120000)
        p0, _, i0 = struct.unpack(">hHI", h[0x10:0x18])
        p1, _, i1 = struct.unpack(">hHI", h[0x20:0x28])
        print(name)
        results = []
        for algo_name, algo in (("IMA", ima), ("Yamaha-B", yamaha_b)):
            for lay, (A, B) in layouts(body, (1, 2, 4, 8, 16, 32, 64, 128, 256)):
                a = subsonic(algo(A, p0, i0))
                b = subsonic(algo(B, p1, i1)) if B is not None else a
                results.append(((a + b) / 2, algo_name, lay))
        for score, algo_name, lay in sorted(results)[:8]:
            print(f"  {score*100:6.2f}%  {algo_name:9} {lay}")


if __name__ == "__main__":
    main()
