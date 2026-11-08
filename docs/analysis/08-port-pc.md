# 08 — Executável do port, pacote local, áudio e distribuição

## Pacote local (`DXPCPAK1`)
- Gerado na máquina do usuário a partir do próprio disco (`dracula-x --install <cue>` ou diálogo na primeira execução). Nada do jogo é distribuído.
- Cabeçalho de 64 bytes: `"DXPCPAK1"`, versão (BE32), nº de setores (BE32), SHA-256 dos setores de usuário (32 bytes); depois os setores de 2048 bytes da faixa de dados em ordem de LBA (`src/saturnfs/package.*`).
- Só o T-9527G V1.400 é aceito: o executável embute o SHA-256 dos setores (`1902b428…2c43`, só o resumo). A geração lança e não deixa arquivo se o resumo não bater; a abertura confere cabeçalho, tamanho e o resumo gravado.
- Gravação em `.tmp` + troca atômica; leitura sequencial sem `seek` (o caso comum do CD). Release: geração em ~2 s (Linux) e ~2,6 s (Windows/Wine), pacotes idênticos byte a byte.

## Executável `dracula-x` (SDL3)
- Pasta de dados: `--data-dir`, ou a do executável se houver um `.pak` lá (modo portátil), ou `SDL_GetPrefPath("dracula-x-pc","dracula-x")`.
- Ritmo: relógio com o período do quadro do núcleo (`kCyclesPerFrame / kMasterHz`), que é o mesmo relógio das amostras do SCSP; a fila de áudio só corrige a deriva (latência alvo ~3 quadros).
- Backup RAM (saves do jogo) gravada a cada ~5 s quando muda e na saída (troca atômica).
- `--frames N` fecha após N quadros (teste de fumaça); `--no-audio`.

## Menu do port, save states e galeria
- Menu sobreposto ao jogo (`src/frontend/menu.*`), aberto com Esc ou Back/Guide; o núcleo fica pausado enquanto ele está aberto.
  Páginas: Save states, Vídeo (janela/tela cheia, escala inteira, proporção, filtro, scanlines, vsync), Áudio (volume, mudo,
  latência), Galeria da trilha sonora, Controles (remapeamento de teclado e controle, zona morta) e Sair. Tudo salvo em `dracula-x.ini`.
- Save states: `states/slotN.dxs` na pasta de dados (gravação atômica). O formato tem versão (`Machine::kStateVersion`, hoje **4**,
  que acrescentou o DSP do SCU, o estado novo do SMPC e a autenticação do CD block na 2 o DSP do SCSP na 3 e os campos XA do diretório do CD block na 4); estados de versão anterior são recusados com
  mensagem, sem derrubar o jogo. Na carga, índices lidos do arquivo (CT do DSP do SCU, conexões e partições do CD block, posição
  de transferência) são validados: um estado corrompido lança `StateError` em vez de escrever fora de um array.
- Galeria (`src/frontend/gallery.*`, `scenes.*`, `gallery_view.cpp`): lista as faixas `SD/SDxx.PCM` lidas do pacote local,
  decodifica o DVI/IMA ADPCM em streaming com laço (`audio/dvi.*`) e mostra um cenário ilustrativo com espectro. O local de uso de
  cada faixa vem da tabela de estágios inferida (`03-tabela-musica.md`); os cenários são arte própria, não imagens do jogo.
- Teste de fumaça do menu: `dracula-x --menu-path "Galeria da trilha sonora" --menu-time 2 --menu-shot arq.png --frames N`.

## Áudio: validação contra as faixas do disco
`dxpc-run … --wav saída.wav` grava a saída do SCSP. Com o roteiro de entrada até o estágio 1 e correlação cruzada contra as faixas decodificadas por `dxpc-music-export`:

| Trecho | Faixa | Correlação (janelas de 0,25 s, alinhadas) |
|---|---|---|
| Menu (≈ quadro 3600) | `SD15` | 0,77 global |
| Estágio 1 (≈ quadro 8750) | `SD19` | 0,965–0,996 (global 0,74 pela deriva abaixo) |

- O resíduo (~18% do RMS) é coerente com os efeitos sonoros mixados por cima.
- Ganho da música ≈ 0,66 da faixa (volume do driver).
- Deriva de ~5,6 amostras/s entre a reprodução e a faixa (0,013%): desprezível.

### Contra o Mednafen (referência gravada por `tools/reference/mednafen_record.sh`, BIOS real, sem entrada)
- Áudio do filme `KONAMI.AVI`: correlação **0,999998** com o Mednafen (6 s, janelas de 0,5 s todas 1,000), com ganho constante de 1,19 (+1,5 dB) do nosso lado. **Causa: o próprio Mednafen**, que depois do SCSP atenua a saída em 27/32 (`bp = (bp * 27 + 16) >> 5` em `src/ss/sound.cpp`, 0,84375 = −1,48 dB; ganho medido 0,836). A escala interna do SCSP (DISDL/EFSDL, envelope, MVOL, saturação em 16 bits) é a mesma nos dois: a nossa saída é a do SCSP digital, sem esse estágio; nada a corrigir. O mesmo fator aparece no jingle do BIOS (abaixo).
- **Jingle "SEGA Saturn" do BIOS** (primeiros ~10 s; antes mudo): o BIOS toca um único slot em laço (PCM de 16 bits que o SH-2 vai reabastecendo) com `DISDL = 0` e `IMXL = 7`; todo o som sai pelo **DSP do SCSP** (programa de reverberação de ~100 passos, anel de atraso em `RBP = 0x18`, `RBL = 2`) e pelas saídas de efeito (`EFSDL = 7`, `EFPAN` diferentes por slot). Sem o DSP, a saída era zero. Com o DSP, o envelope e o início coincidem com o Mednafen e a correlação por canal é de **0,99996** (SNR ≈ 41 dB, limitado pelo reamostrador linear da comparação) com o ganho de 27/32 acima. O DSP foi verificado também contra uma transcrição do `RunDSP` de referência: 0 divergências em 20 000 amostras com o programa do BIOS.
- **Pan (DIPAN/EFPAN)**: a comparação acima só fechou depois de inverter o sentido do bit 4: com ele desligado atenua-se o canal **esquerdo** (0x0F = tudo à direita) e ligado o **direito** (0x1F = tudo à esquerda), como no Mednafen; antes estava ao contrário (canal esquerdo nosso = direito do Mednafen, ρ = 1,000). Consequência: a saída do jogo (WAV do HLE) passa a ter L e R trocados em relação à versão anterior (conferido: idêntica com os canais invertidos). O jogo põe DIPAN = 0x0F no slot 0 e 0x1F no slot 1 do fluxo da música.
- O título é mudo nos dois; tela de título idêntica em conteúdo (inclusive "PRESS START BUTTON").

### Recursos do SCSP usados pelo jogo
Censo (instrumentação temporária de todas as escritas do 68000 nos registradores do SCSP, ~11 mil quadros do boot ao estágio 1 com
chicote/pulo): **nenhuma** escrita de FM (MDL), LFO, SSCTL≠0 ou programa/coeficientes do DSP; só 3 escritas de envio a efeitos
(IMXL) que não soam (MPRO vazio, EFSDL = 0). No gameplay só tocam 2 slots (o fluxo da música, L e R separados por DIPAN). Logo
FM e LFO — ainda não emulados — não afetam o trecho alcançado (o BIOS também não os usa); o DSP já é emulado por causa do BIOS; se áreas posteriores carregarem um programa de DSP, o mesmo censo
o revela.

## Temporização NTSC
Modelo do hardware (`src/saturn/video_timing.*`): 263 linhas por quadro (não entrelaçado); linha de 427 pontos no modo de 320
(relógio da CPU 26,846590 MHz, 1708 ciclos) ou 455 no de 352 (28,636360 MHz, 1820 ciclos), conforme o HRESO do TVMD; HBlank ao fim
dos pontos ativos (ciclo 1280/1408); VBlank ao fim das linhas visíveis do VRESO (224 ou 240 — o estágio usa 240). O SCSP tem relógio
próprio (44,1 kHz) e converte ciclos da CPU em amostras pelo relógio do modo atual (`Scsp::set_master_hz`). `Machine::run_frame()`
roda até o próximo quadro e `frame_seconds()` dá o período real (≈59,77 Hz no modo de 320), usado pelo ritmo do executável.

Antes era 262 × 1820 a 28,64 MHz fixo (60,05 Hz) com VBlank sempre na linha 224 — errado para o modo de 240 linhas do jogo.
Validação após a mudança: tour de cenas idêntico em conteúdo, 737,9 amostras por quadro (= 44100/59,77), áudio do filme da Konami
ainda ρ = 1,000 contra o Mednafen e a música do estágio ρ 0,95–0,99 contra `SD19`; save states continuam determinísticos.

## Distribuição
- `tools/package/build-linux.sh` → `dist/dracula-x-linux-x86_64.tar.gz` (libSDL3 embutida com rpath `$ORIGIN/lib`; exige glibc ≥ 2.38 da máquina de build).
- `tools/package/build-windows.sh` → `dist/dracula-x-windows-x64.zip` (mingw-w64, runtime C++ estático, só o `SDL3.dll` ao lado). Verificado no Wine 11: instalação a partir do `.cue`, jogo com áudio e vídeo.
- `tools/package/build-appimage.sh` → `dist/Dracula-X-PC-x86_64.AppImage` (mesmo executável, libSDL3 em `usr/bin/lib`, ícone original em `tools/package/icon.png`). Testado com `APPIMAGE_EXTRACT_AND_RUN=1`; em máquinas com FUSE basta executá-lo.
- `tools/package/build-windows-installer.sh` → `dist/Dracula-X-PC-Setup-x64.exe` (NSIS: instala em Arquivos de Programas, atalhos no menu Iniciar e desinstalador; os dados do usuário em `%APPDATA%` não são apagados). Instalação e desinstalação silenciosas testadas no Wine. Não é assinado digitalmente.
