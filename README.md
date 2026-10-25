# Dracula X PC (port nativo)

Port nativo para PC (Linux/Windows) do jogo de Sega Saturn *Akumajou Dracula X – Gekka no Yasoukyoku*
(T-9527G v1.400), gerado localmente a partir da **sua própria imagem de disco**.

Este repositório contém apenas código e ferramentas originais. **Nenhum dado do jogo é distribuído.**

Design: `docs/superpowers/specs/`. Plano atual: `docs/superpowers/plans/`.

## Ferramentas (linha de comando)
Compile com `cmake -S . -B build-local -G Ninja && cmake --build build-local`. Todas recebem o `.cue` do **seu** disco e recusam qualquer outro que não seja o T-9527G V1.400 completo.

| Ferramenta | Faz |
|---|---|
| `dxpc-extract <cue> <pasta> [--force]` | extrai os arquivos do disco para uma pasta local + `manifest.txt` |
| `dxpc-catalog <cue>` | lista e classifica os arquivos por tipo |
| `dxpc-overlays <cue>` | endereço de carga dos overlays `.PRG` e sobreposições |
| `dxpc-music-export <cue> <pasta>` | decodifica as 49 faixas de música para `.wav` (≈1,1 GB) |

Scripts de análise (`tools/analysis/*.py`) usam Python 3 com `numpy` e `Pillow`; os de `tools/ghidra/` usam o Ghidra 12.2 (ver `tools/ghidra/README.md`). Resultados em `docs/analysis/`.
