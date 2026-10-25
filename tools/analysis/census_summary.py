"""Resume data-local/ghidra/census.txt (linhas `CENSO <programa> chave=valor ...`) em tabelas Markdown e totais.

Uso: python3 -I tools/analysis/census_summary.py data-local/ghidra/census.txt
"""
import sys


def parse(path):
    rows = []
    for line in open(path):
        if not line.startswith("CENSO "):
            continue
        name, *pairs = line.split()[1:]
        rows.append((name, {k: int(v) for k, v in (p.split("=") for p in pairs)}))
    return rows


def main():
    rows = parse(sys.argv[1])
    keys = ["funcoes", "instrucoes", "jsr_indireto", "jmp_indireto", "tabelas_de_salto"]
    hardware = ["smpc", "minit", "sinit", "scu_dma", "scu_dsp", "vdp1_regs", "vdp1_vram", "vdp2_regs", "vdp2_vram", "vdp2_cram",
                "scsp", "cd_block", "sh2_onchip"]
    print("| Programa | " + " | ".join(keys) + " |\n|---|" + "---|" * len(keys))
    for name, v in rows:
        print(f"| `{name}` | " + " | ".join(str(v[k]) for k in keys) + " |")
    total = {k: sum(v[k] for _, v in rows) for k in keys + hardware + ["bytes"]}
    print(f"\n**Totais ({len(rows)} programas):** " + ", ".join(f"{k}={total[k]}" for k in keys))
    print("\n**Literais de hardware (total / programas que usam):**")
    for k in hardware:
        users = sum(1 for _, v in rows if v[k] > 0)
        print(f"- `{k}`: {total[k]} literais em {users} programa(s)")
    indirect_other = total["jmp_indireto"] - total["tabelas_de_salto"]
    print(f"\nSaltos indiretos que NÃO são tabela de salto (candidatos a alvo dinâmico): {indirect_other} de {total['jmp_indireto']}")


if __name__ == "__main__":
    main()
