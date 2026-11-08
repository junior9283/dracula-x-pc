# Dracula X PC (emulador dedicado)

Emulador dedicado, para PC (Linux, Windows e macOS), do jogo de Sega Saturn *Akumajou Dracula X – Gekka no Yasoukyoku*
(T-9527G v1.400), com um aplicativo completo em volta: janela, controles, menu, save states e galeria da trilha sonora.
Funciona a partir da **sua própria imagem de disco**.

**O que é e o que não é.** O que roda nativamente no PC é o emulador e o aplicativo. **O jogo em si não é nativo**: o programa original do Saturn
(código de máquina SH-2 e driver de som 68000, lidos do seu disco) é *interpretado* por um núcleo escrito do zero em C++. Não é um port recompilado.
Converter esse código em código de PC (recompilação estática) é a meta de longo prazo e **ainda não foi feita**.

Este repositório contém apenas código e ferramentas originais. **Nenhum dado do jogo é distribuído.**

**Aviso legal.** Projeto independente e sem fins comerciais, não afiliado nem endossado pela Konami nem pela Sega. *Castlevania*, *Akumajou Dracula X*, *Sega Saturn* e demais nomes são marcas de seus donos. O repositório e os pacotes não incluem o jogo, a imagem do disco, o pacote gerado a partir dele nem nenhuma BIOS: é preciso ter o seu próprio disco (T-9527G v1.400) e usá-lo conforme a lei do seu país. A BIOS é substituída por uma implementação própria; o código foi escrito a partir de documentação pública e de observação do comportamento do jogo, sem copiar código de outros emuladores. Licença do código: MIT (`LICENSE`).

**Página do projeto:** https://junior9283.github.io/dracula-x-pc/ · **Downloads:** [releases](https://github.com/junior9283/dracula-x-pc/releases)

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
- macOS: `tools/package/build-macos.sh` (num Mac; gera `Dracula-X-PC-macOS-{universal,x86_64,arm64}.dmg`) ou `.github/workflows/macos.yml` (compilação no GitHub Actions). Não testado em Mac real: veja `docs/analysis/08-port-pc.md`.
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
