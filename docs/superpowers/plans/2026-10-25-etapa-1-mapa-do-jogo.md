# Etapa 1 — Mapa do jogo: arquivos, overlays, música, gráficos, som e vídeo

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Entender e documentar (e onde couber, ler por código) todos os tipos de dado do disco e como o executável carrega overlays, de modo que a etapa 2 (núcleo SH-2) comece com um mapa confiável.

**Architecture:** Tarefas de ferramenta (C++ em `src/saturnfs/` e `src/tools/`, com TDD sobre dados sintéticos) alternam com tarefas de análise (Ghidra headless sobre `data-local/disc`, resultado em `docs/analysis/`). Nenhum dado do jogo entra no repositório; testes usam bytes sintéticos construídos no próprio teste.

**Tech Stack:** C++20, CMake/Ninja, Catch2 (já no projeto), Ghidra 12.2 headless (`~/opt/ghidra_12.2_PUBLIC`, scripts Java em `tools/ghidra/`), ffmpeg/ffprobe (só para verificação local).

**Spec:** `docs/superpowers/specs/2026-10-25-dracula-x-pc-design.md` · **Etapa anterior:** `docs/analysis/00-boot-e-memoria.md`

## Global Constraints

- C++20, Linux e Windows; licença MIT; código escrito do zero (emuladores só como referência de comportamento).
- Nenhum dado do jogo no repositório ou em releases; `data-local/`, `build-local/` ignorados pelo git.
- Disco aceito: T-9527G V1.400 (faixa de dados com 204813 setores).
- Documentos de análise separam **[obs]** observado, **[inf]** inferido e **[pad]** convenção; todo número tem como reproduzir (script em `tools/` ou comando no texto).
- Nunca `git push` sem pedido explícito.

## Fatos de partida (etapa 0 + exploração)

- 436 arquivos; `0.BIN` em `0x06004080` (713 funções); `*.PRG` começam com tabela de ponteiros `0x060Exxxx` (região de overlays) [obs].
- `SD/`: 62 arquivos (296 MB). 49 (`SD01`–`SD31`, hex) têm cabeçalho `DVI.` de 0x800 bytes: `+4=0x800`, `+8=tamanho do corpo` (tamanho do arquivo − 0x800), `+0xC`, `+0x10`, `+0x14` ainda sem significado [obs]. Os demais (`SDD*`, `SDE*`, `SDF0`) sem magic; `SD_*.CRT` são outro tipo [obs]. Hipótese [inf]: `SD01–SD31` = faixas de música em fluxo.
- `*.AVI` (4): vídeo `truemotion1`, áudio `adpcm_ima_dk4` 44,1 kHz estéreo; ffmpeg decodifica [obs].
- `OMAKE/24`, `OMAKE/8`: BMPs (576×768, 24 e 8 bpp) e `OMAKE/KAIHATSU.DOC` (texto Shift-JIS) [obs].
- `0.BIN` contém tabela de nomes de arquivo (270 nomes `*.CHR/.MAP/.PRG/.PCM/.PTN`) entre `0x13D8` e `0x16B70` [obs].

## Review Focus

- Nome de arquivo em minúsculas ou com extensão desconhecida: classificado como `Unknown`, sem exceção.
- Arquivo vazio ou menor que o cabeçalho esperado (DVI < 0x800, overlay < tabela): erro claro, sem leitura fora do buffer.
- Campo de tamanho do cabeçalho DVI inconsistente com o tamanho do arquivo: rejeitado com mensagem.
- Decodificação de áudio com buffer de entrada de tamanho ímpar/truncado: não lê além do fim e informa quantas amostras produziu.
- Tabela de overlays com dois arquivos mapeados para a mesma região: sinalizado como conflito na ferramenta, não silenciado.

---

### Task 1: Catálogo do disco

**Files:**
- Create: `src/saturnfs/catalog.hpp/.cpp`, `src/tools/catalog_main.cpp`
- Modify: `CMakeLists.txt` (alvo `dxpc-catalog`)
- Test: `tests/catalog_test.cpp`

**Interfaces:**
- Consumes: `list_files`, `read_file`, `FileEntry` (etapa 0).
- Produces:
  - `enum class FileKind { MainExecutable, Overlay, Graphics, TileMap, Pattern, MusicStream, SoundBank, SoundRaw, Video, Bitmap, Font, Text, Unknown };`
  - `FileKind classify(std::string_view path, std::span<const uint8_t> head);` (por nome — sem diferenciar maiúsculas — e por magic: `DVI.` em `SD/*.PCM` → `MusicStream`, `SD/*.PCM` sem magic → `SoundRaw`, `SD_*.CRT` → `SoundBank`, `RIFF....AVI ` → `Video`, `BM` → `Bitmap`, `0.BIN` → `MainExecutable`, `*.PRG` → `Overlay`, `.CHR` → `Graphics`, `.MAP` → `TileMap`, `.PTN` → `Pattern`, `.FON` → `Font`, `.TXT`/`.DOC` → `Text`)
  - `struct CatalogEntry { FileEntry file; FileKind kind; };` e `std::vector<CatalogEntry> build_catalog(const DiscImage&);`
  - CLI: `dxpc-catalog <cue>` imprime `tipo tamanho caminho` por linha e totais por tipo.

- [ ] **Step 1:** Testes `classify`: `("SD/SD01.PCM", "DVI."+…)` → `MusicStream`; `("SD/SDE0.PCM", zeros)` → `SoundRaw`; `("stage_00.prg", …)` → `Overlay`; `("X.ZZZ", …)` → `Unknown`; `("0.BIN", …)` → `MainExecutable`; cabeçalho vazio com nome `SD/SD01.PCM` → `SoundRaw`.
- [ ] **Step 2:** Run `ctest --test-dir build-local` — Expected: FAIL (símbolos não definidos).
- [ ] **Step 3:** Implementar `classify` e `build_catalog` (lê só os primeiros 16 bytes de cada arquivo).
- [ ] **Step 4:** Run — Expected: PASS. Rodar `dxpc-catalog` no disco real: 436 linhas, 49 `MusicStream`, 4 `Video`.
- [ ] **Step 5:** Commit `feat: catálogo de arquivos do disco`.

### Task 2: Carregador de arquivos e mapa de overlays

**Files:**
- Create: `docs/analysis/01-overlays.md`, `tools/ghidra/FileLoaderTrace.java`, `src/saturnfs/overlay.hpp/.cpp`, `src/tools/overlays_main.cpp`
- Test: `tests/overlay_test.cpp`

**Interfaces:**
- Consumes: `FileEntry`, `read_file`.
- Produces (assinatura fixa; campos preenchidos conforme o formato documentado no passo 3):
  - `struct OverlayInfo { std::string name; uint32_t load_address; uint32_t size; std::vector<uint32_t> header_table; };`
  - `OverlayInfo parse_overlay(std::string_view name, std::span<const uint8_t> bytes);` (lança `std::runtime_error` se o arquivo for menor que a tabela de cabeçalho)
  - `std::vector<std::string> find_overlay_conflicts(std::span<const OverlayInfo>);` (nomes de pares com regiões `[load_address, +size)` sobrepostas **e** que não são alternativas mutuamente exclusivas documentadas)

- [ ] **Step 1:** No Ghidra (`0.BIN`, base `0x06004080`): achar as referências ao bloco de strings `0x060070d4` (`ALC_W.CHR` e vizinhas) e a rotina que as usa para ler o CD; registrar endereço, argumentos e como o endereço de destino é obtido. Salvar o trecho de análise em `docs/analysis/01-overlays.md`.
- [ ] **Step 2:** Verificação: o destino de carga de `STAGE_00.PRG` e de `TITLE.PRG` extraído do código coincide com `min(header_table)` ou com o valor documentado; se não coincidir, registrar `Ruling` e usar o que o código mostra.
- [ ] **Step 3:** Escrever `overlay_test.cpp` com overlay sintético (tabela de 10 ponteiros `0x060E….`, seguida de zeros): `parse_overlay` devolve `header_table.size() == 10` e o `load_address` segundo a regra documentada; arquivo de 4 bytes lança; `find_overlay_conflicts` acusa dois overlays com regiões sobrepostas.
- [ ] **Step 4:** Run — Expected: FAIL; implementar; Run — Expected: PASS.
- [ ] **Step 5:** `dxpc-overlays <cue>` imprime nome, endereço de carga, tamanho e conflitos para todos os `*.PRG`; conferir que `RSTAGEnn` (castelo invertido) e `STAGE_nn` ocupam a mesma região mas são exclusivos (documentar). Commit.

### Task 3: Música em fluxo (`SD01`–`SD31`)

**Files:**
- Create: `src/audio/dvi.hpp/.cpp`, `src/tools/music_export_main.cpp`, `docs/analysis/02-musica-dvi.md`
- Modify: `CMakeLists.txt`
- Test: `tests/dvi_test.cpp`

**Interfaces:**
- Produces:
  - `struct DviHeader { uint32_t header_size; uint32_t body_size; uint32_t field_0c; uint32_t field_10; uint32_t field_14; };` (nomes finais dos campos vêm da análise)
  - `DviHeader parse_dvi_header(std::span<const uint8_t> file);` (lança se `< 0x800` bytes, magic ≠ `DVI.` ou `header_size + body_size ≠ file.size()`)
  - `std::vector<int16_t> decode_dvi(std::span<const uint8_t> body, const DviHeader&);` (retorna PCM 16 bits; entrada truncada produz o que for possível sem ler além do fim)
  - `dxpc-music-export <cue> <pasta>`: grava um `.wav` por faixa em `data-local/music/` (local, ignorado pelo git).

- [ ] **Step 1:** No Ghidra, achar a rotina que lê `SD##.PCM` e a que consome o fluxo (decodificação ou envio ao SCSP): determinar algoritmo (suspeita [inf]: IMA/DVI ADPCM), taxa de amostragem, canais, significado dos campos `+0xC/+0x10/+0x14` (laço? estado inicial?). Registrar em `docs/analysis/02-musica-dvi.md` com os endereços das rotinas.
- [ ] **Step 2:** Testes: `parse_dvi_header` aceita cabeçalho sintético válido (0x800 B + corpo de 16 B) e lança para magic errado, arquivo de 100 B e `body_size` inconsistente; `decode_dvi` de um corpo sintético de bytes zero devolve silêncio com o número de amostras esperado pela regra documentada; entrada de tamanho ímpar não excede o buffer.
- [ ] **Step 3:** Run — Expected: FAIL. Implementar `parse_dvi_header` e `decode_dvi`; Run — Expected: PASS.
- [ ] **Step 4:** Verificação no disco real: exportar `SD01`; `ffprobe` informa duração coerente com `body_size`/taxa; ouvir/inspecionar que o sinal não é ruído (energia por janela varia suavemente; sem clipping contínuo).
- [ ] **Step 5:** Commit.

### Task 4: Tabela música → local

**Files:**
- Create: `docs/analysis/03-tabela-musica.md`, `tools/ghidra/MusicRefs.java`

**Interfaces:**
- Consumes: resultados das tasks 2 e 3 (nomes de overlay, rotina de carga de `SD##`).
- Produces: tabela `faixa (SD##) → overlay/fase que a toca → nome do local` em `docs/analysis/03-tabela-musica.md`, base para o tema visual da galeria (etapa 7).

- [ ] **Step 1:** Em cada overlay `*.PRG`, listar as chamadas à rotina de seleção de música e o índice passado (script `MusicRefs.java`).
- [ ] **Step 2:** Verificação: as faixas encontradas cobrem pelo menos 40 dos 49 `SD01–SD31`; as sem referência ficam listadas como "não referenciadas por código estático" (podem vir por tabela de dados).
- [ ] **Step 3:** Para os locais, usar nomes dos overlays (`LIBRARY`, `STAGE_nn`, `BOSS_nn`, `TITLE`, `OPEN`, `STAFF` …) e deixar o nome de exibição como `[inf]` quando não houver texto no jogo.
- [ ] **Step 4:** Commit do documento e do script.

### Task 5: Gráficos (`.CHR`, `.MAP`, `.PTN`)

**Files:**
- Create: `docs/analysis/04-graficos.md`, `src/tools/chr_dump_main.cpp`, `src/gfx/chr.hpp/.cpp`
- Test: `tests/chr_test.cpp`

**Interfaces:**
- Produces: `std::vector<uint8_t> decode_chr_4bpp(std::span<const uint8_t> tiles, int tile_w, int tile_h);` (ou a variante que a análise confirmar: 4/8/16 bpp, tamanho de tile) e `dxpc-chr-dump <arquivo> <png>` (gera PNG local para inspeção, ignorado pelo git).

- [ ] **Step 1:** Observar nas rotinas de carga/DMA do `0.BIN` e dos overlays para onde cada tipo vai (VDP1 VRAM, VDP2 VRAM, Color RAM); documentar layout de `.CHR` (ex.: `STAGE_00.CHR` começa com pares `0000 000d 0000 0000 00e0 000c …`, possível tabela de offsets/tamanhos), `.MAP` (compressão? `STAGE_00.MAP` começa `fd00 de7e 4445…`) e `.PTN` (`S011.PTN` começa `0001 0002 0003 …`: índices de quadros de animação).
- [ ] **Step 2:** Teste: `decode_chr_4bpp` de 2 tiles sintéticos (valores de nibble conhecidos) devolve os índices esperados na ordem de varredura documentada; entrada menor que um tile não lê fora do buffer.
- [ ] **Step 3:** Run — FAIL; implementar; Run — PASS.
- [ ] **Step 4:** Verificação visual local: gerar PNG de `S011.CHR` com a paleta mais plausível e confirmar que há formas coerentes (não ruído); registrar a paleta/Color RAM usada.
- [ ] **Step 5:** Commit.

### Task 6: Efeitos sonoros, driver de som e vídeo/bitmaps

**Files:**
- Create: `docs/analysis/05-som-e-midia.md`

**Interfaces:**
- Produces: documento com (a) formato de `SD_*.CRT` e dos `SDD*/SDE*/SDF0` (cru? 8/16 bits? taxa?), (b) onde o programa do 68000 do SCSP vem (arquivo, endereço, tamanho) e quais registradores o SH-2 usa para comandá-lo, (c) receita de conversão dos `.AVI` e dos BMPs de `OMAKE` para formatos de PC.

- [ ] **Step 1:** Localizar no `0.BIN` o código que escreve em `0x05A00000–0x05BFFFFF` (SCSP) e o upload do programa 68K; registrar endereços.
- [ ] **Step 2:** Para cada família de som, identificar o formato por cabeçalho e uso no código; validar convertendo uma amostra com ffmpeg (`-f s16be -ar <taxa>`) e inspecionando a forma de onda.
- [ ] **Step 3:** Vídeos: confirmar com `ffprobe` os quatro `.AVI` (`truemotion1` + `adpcm_ima_dk4`), registrar resolução real do fluxo (o `ffprobe` informa 140×144 para `OPENING.AVI`, divergente do 280×144 do cabeçalho `avih`: decidir qual vale) e comando de conversão para o pacote local (ex.: `ffmpeg -i X.AVI -c:v libx264 -c:a aac X.mp4`).
- [ ] **Step 4:** Commit.

### Task 7: Censo de código e mapa consolidado

**Files:**
- Create: `tools/ghidra/CodeCensus.java`, `docs/analysis/06-mapa-do-jogo.md`

**Interfaces:**
- Consumes: documentos 01–05.
- Produces: `06-mapa-do-jogo.md` com tabela por overlay (tamanho, funções, saltos indiretos `jmp @rN`/`jsr @rN`, escritas na própria região de código), uso do SH-2 escravo (acessos ao SMPC/SCU para ligá-lo), uso de VDP1/VDP2/SCU DSP/DMA, e uma recomendação fundamentada sobre a viabilidade da recompilação estática para a etapa 2.

- [ ] **Step 1:** `CodeCensus.java` percorre o programa aberto e imprime: nº de funções, nº de chamadas/saltos indiretos, nº de instruções que gravam em endereços dentro da própria região do arquivo.
- [ ] **Step 2:** Rodar para `0.BIN` e para **todos** os `*.PRG` (cada um na base da task 2); coletar numa tabela.
- [ ] **Step 3:** Verificação: totais batem com contagem independente (ex.: `0.BIN` = 713 funções antes de criar a entrada); divergências explicadas.
- [ ] **Step 4:** Escrever recomendação: recompilação estática pura, híbrida (com interpretador) ou interpretador primeiro, com os números que a sustentam. Atualizar a seção "Decisões" do design se algo mudar.
- [ ] **Step 5:** Commit.
