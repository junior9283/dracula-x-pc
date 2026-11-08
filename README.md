# Dracula X PC (port nativo)

Port nativo para PC (Linux/Windows) do jogo de Sega Saturn *Akumajou Dracula X – Gekka no Yasoukyoku*
(T-9527G v1.400), gerado localmente a partir da **sua própria imagem de disco**.

Este repositório contém apenas código e ferramentas originais. **Nenhum dado do jogo é distribuído.**

Design: `docs/superpowers/specs/`. Plano atual: `docs/superpowers/plans/`.

## Jogar
Compile com `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release` (precisa do SDL3) e rode `build-release/dracula-x`.
Na primeira execução ele pede o `.cue` do seu disco (ou `dracula-x --install disco.cue`), confere o SHA-256 dos setores do
T-9527G V1.400 e gera o pacote local `dracula-x.pak` (formato `DXPCPAK1`, ~400 MB) na pasta de dados; depois disso o disco não é mais usado.
Saves (`dracula-x.bup`), capturas e configurações ficam na mesma pasta. Detalhes em `docs/analysis/08-port-pc.md`.

Durante o jogo: **Esc** (ou Back/Guide do controle) abre o menu do port — save states (8 slots), vídeo, áudio,
remapeamento de teclado e controle, **galeria da trilha sonora** e sair. Atalhos: F5/F7 salva/carrega o estado, F6/F8 trocam o
slot, F11 alterna tela cheia, F12 grava uma captura. As opções ficam em `dracula-x.ini` na pasta de dados.

Pacotes de distribuição (sem dados do jogo), em `dist/`:
- Linux: `tools/package/build-linux.sh` (tar.gz com a libSDL3 embutida) e `tools/package/build-appimage.sh` (`Dracula-X-PC-x86_64.AppImage`, arquivo único; baixa o `appimagetool` oficial para `build-appimage/` na primeira vez).
- Windows: `tools/package/build-windows.sh` (zip portátil por cross-compilação mingw-w64; o SDL3 do Windows é baixado por `fetch-sdl3-mingw.sh`, sem root) e `tools/package/build-windows-installer.sh` (`Dracula-X-PC-Setup-x64.exe`, instalador NSIS; precisa do `makensis`).

## Ferramentas (linha de comando)
Compile com `cmake -S . -B build-local -G Ninja && cmake --build build-local`. Todas recebem o `.cue` do **seu** disco e recusam qualquer outro que não seja o T-9527G V1.400 completo.

| Ferramenta | Faz |
|---|---|
| `dxpc-extract <cue> <pasta> [--force]` | extrai os arquivos do disco para uma pasta local + `manifest.txt` |
| `dxpc-catalog <cue>` | lista e classifica os arquivos por tipo |
| `dxpc-overlays <cue>` | endereço de carga dos overlays `.PRG` e sobreposições |
| `dxpc-music-export <cue> <pasta>` | decodifica as 49 faixas de música para `.wav` (≈1,1 GB) |

Scripts de análise (`tools/analysis/*.py`) usam Python 3 com `numpy` e `Pillow`; os de `tools/ghidra/` usam o Ghidra 12.2 (ver `tools/ghidra/README.md`). Resultados em `docs/analysis/`.
