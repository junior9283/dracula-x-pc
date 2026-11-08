#!/usr/bin/env python3
"""Extrai uma captura da fonte DESCOMPACTADA da Work RAM-H do Sega Saturn.

O arquivo de entrada deve ser um dump bruto de 1 MiB a partir de 0x06000000,
obtido de uma sessão de referência *após* a BIOS preparar 0x06000E20.
Não use os bytes brutos da ROM 0x5456: a etapa que prepara a fonte ainda
não foi traduzida para C++ e o formato original não está confirmado.

Uso: python3 tools/extract_boot_font.py data-local/ram-bios.bin data-local/boot-font.bin
"""

import argparse
from pathlib import Path

WORK_RAM_H_SIZE = 0x100000
FONT_OFFSET = 0x0E20
FONT_SIZE = 0x1000


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ram_dump", type=Path, help="dump de 1 MiB da Work RAM-H")
    parser.add_argument("output", type=Path, help="arquivo local de 4096 bytes da fonte")
    args = parser.parse_args()

    data = args.ram_dump.read_bytes()
    if len(data) != WORK_RAM_H_SIZE:
        parser.error(f"tamanho da RAM inválido: esperado {WORK_RAM_H_SIZE} bytes, obtido {len(data)}")
    font = data[FONT_OFFSET : FONT_OFFSET + FONT_SIZE]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(font)
    print(f"fonte extraída: {args.output} ({len(font)} bytes, origem 0x06000E20)")


if __name__ == "__main__":
    main()
