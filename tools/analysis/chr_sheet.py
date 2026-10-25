"""Verificação visual local de um .CHR cru de 4 bpp: gera uma folha PNG com os sprites w x h empilhados.

Formato validado em ITEM.CHR: linhas de w/2 bytes, nibble alto = pixel à esquerda, sprites consecutivos (cada um h linhas).
A paleta é ARBITRÁRIA (o jogo usa a Color RAM do VDP2/VDP1); serve só para ver a forma.
Uso: python3 -I tools/analysis/chr_sheet.py <arquivo.CHR> <saida.png> [largura=16] [altura=16] [colunas=20] [linhas-de-icones=12]
"""
import colorsys
import sys

import numpy as np
from PIL import Image


def main():
    path, out = sys.argv[1], sys.argv[2]
    w, h, cols, rows = (int(v) for v in (sys.argv[3:7] + [16, 16, 20, 12][len(sys.argv) - 3:])) if len(sys.argv) > 3 else (16, 16, 20, 12)
    raw = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
    px = np.empty(len(raw) * 2, np.uint8)
    px[0::2], px[1::2] = raw >> 4, raw & 15
    sprites = len(px) // (w * h)
    sheet = np.zeros((rows * h, cols * w), np.uint8)
    for i in range(min(sprites, rows * cols)):
        r, c = divmod(i, cols)
        sheet[r * h:(r + 1) * h, c * w:(c + 1) * w] = px[i * w * h:(i + 1) * w * h].reshape(h, w)
    pal = np.array([[0, 0, 0]] + [[int(255 * v) for v in colorsys.hsv_to_rgb(i / 15, 0.6, 0.55 + 0.4 * (i % 3) / 2)]
                                   for i in range(1, 16)], dtype=np.uint8)
    Image.fromarray(pal[sheet]).resize((cols * w * 3, rows * h * 3), Image.NEAREST).save(out)
    print(f"{sprites} sprites {w}x{h}; folha com {min(sprites, rows * cols)} em {out}")


if __name__ == "__main__":
    main()
