# Dracula X PC — instruções do projeto

Emulador dedicado (em evolução para o jogo nativo de PC) de *Akumajou Dracula X* (Sega Saturn, T-9527G v1.400). Escreva e converse em **português do Brasil**.

## Objetivo atual
O **jogo em si** nativo de PC (recompilação do código SH-2 e do driver de som 68000). Plano: `docs/superpowers/plans/2026-11-08-recompilacao-nativa.md`.
Andamento: `docs/recompilacao/DIARIO.md`. Erros: `docs/recompilacao/ERROS.md`.

## Como trabalhar
- **Documente conforme avança:** uma entrada no `DIARIO.md` a cada passo concluído (o que, medições, resultado, próximo passo).
- **Erros:** avalie e resolva por conta própria, sem esperar o usuário. Ciclo: sintoma → causa → correção → teste que reproduz → lição, registrado em `ERROS.md`. Só pergunte quando a decisão for dele (arquitetura de entrega, licença, o que vai a público).
- **Nada do jogo no repositório nem nos pacotes:** imagem do disco, `.pak`, BIOS, arquivos extraídos e o código gerado a partir do jogo ficam fora (`data-local/`, `dist/` e `build-*/` já são ignorados). Nunca imprima nem comite a BIOS.
- **Identidade bit a bit:** mudanças de desempenho ou de tradução não podem alterar a saída. Prove com `tools/reference/identidade.sh <dxpc-run> <pasta>` (imagem, áudio e estado completo) antes e depois, e compare com `cmp`.
- **Testes:** `build-release/tests/tests` (382 casos). Toda correção tem um teste que falha sem ela. Não altere um teste correto para fazer código errado passar.
- **Git:** só faça commit ou push quando o usuário pedir.

## Comandos
- Build: `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`
- Execução sem janela: `dxpc-run <cue> --lenient-bios --boot-font data-local/boot-font.bin --frames N [--pad ...] [--shot q:arq.png] [--wav a.wav] [--frame-log f.csv] [--coverage] [--poke q:end=valor]`. O roteiro de entrada até o gameplay está em `tools/reference/pad-gameplay.txt`.
- Medir: `python3 tools/analysis/frame_times.py f.csv 8400 9600`.
- Pacotes: `tools/package/build-{linux,appimage,windows,windows-installer,macos}.sh`. Publicação: Releases e página (`docs/index.html`) no GitHub `junior9283/dracula-x-pc`.
