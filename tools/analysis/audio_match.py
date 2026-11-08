#!/usr/bin/env python3
"""Fidelidade de áudio: acha, entre WAVs de referência, o trecho que corresponde a um pedaço da gravação do port, e mede a
correlação em janelas curtas (alinhamento refeito em cada janela, para separar deriva de diferença real) e o ganho relativo.

Uso: python3 -I tools/analysis/audio_match.py <gravação.wav> <início-s> <duração-s> <referência.wav | pasta-de-wavs> [--windows]
  ex.: dxpc-run <cue> ... --wav /tmp/port.wav ; python3 -I tools/analysis/audio_match.py /tmp/port.wav 146 4 data-local/music --windows
Referências com outra taxa de amostragem devem ser convertidas antes (ex.: ffmpeg -i ref.wav -ar 44100 ref441.wav).
Requer numpy.
"""
import pathlib
import sys
import wave

import numpy as np


def load(path):
    with wave.open(str(path)) as w:
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        return data.reshape(-1, w.getnchannels())[:, 0], w.getframerate()


def best_offset(reference, segment):
    n = 1 << int(np.ceil(np.log2(len(reference) + len(segment))))
    corr = np.fft.irfft(np.fft.rfft(reference, n) * np.conj(np.fft.rfft(segment - segment.mean(), n)), n)
    return int(np.argmax(corr[: len(reference) - len(segment)]))


def correlation(a, b):
    a, b = a - a.mean(), b - b.mean()
    return float(np.dot(a, b) / np.sqrt(np.dot(a, a) * np.dot(b, b) + 1e-9))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 4:
        sys.exit(__doc__)
    recording, rate = load(args[0])
    start, duration = float(args[1]), float(args[2])
    segment = recording[int(start * rate): int((start + duration) * rate)]
    target = pathlib.Path(args[3])
    candidates = sorted(target.glob("*.wav")) if target.is_dir() else [target]

    results = []
    for path in candidates:
        reference, ref_rate = load(path)
        if ref_rate != rate or len(reference) < len(segment):
            continue
        k = best_offset(reference, segment)
        results.append((correlation(reference[k: k + len(segment)], segment), path, k))
    if not results:
        sys.exit("nenhuma referência compatível (mesma taxa e mais longa que o trecho)")
    results.sort(key=lambda r: r[0], reverse=True)
    for rho, path, k in results[:4]:
        print(f"rho={rho:.4f} {path.name} deslocamento={k / rate:.2f}s")

    if "--windows" in sys.argv:
        _, path, k = results[0]
        reference, _ = load(path)
        window, search = rate // 4, rate // 100
        for i in range(0, len(segment) - window, window):
            piece = segment[i: i + window]
            lo = max(0, k + i - search)
            span = reference[lo: k + i + window + search]
            j = lo + best_offset(span, piece)
            ref_piece = reference[j: j + window]
            gain = np.dot(piece, ref_piece) / max(np.dot(ref_piece, ref_piece), 1.0)
            print(f"t={start + i / rate:7.2f}s rho={correlation(ref_piece, piece):.3f} desvio={j - (k + i):+5d} ganho={gain:.3f}")


if __name__ == "__main__":
    main()
