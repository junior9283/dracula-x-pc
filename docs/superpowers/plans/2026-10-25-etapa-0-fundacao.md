# Etapa 0 — Fundação: repositório, leitor de disco e análise inicial

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ter um repositório C++20 que lê o `.cue/.bin` do jogo, valida o disco, extrai os arquivos do ISO9660 para uma pasta local e deixa um projeto Ghidra/REA com o executável de boot carregado e o mapa de memória inicial documentado.

**Architecture:** Biblioteca `saturnfs` (cue → faixas → setores → ISO9660) usada por uma CLI `dxpc-extract`. Testes unitários com disco sintético gerado em memória (nenhum dado do jogo entra no repositório). A análise no Ghidra/REA é feita sobre os arquivos extraídos em `data-local/` (ignorado pelo git).

**Tech Stack:** C++20, CMake + Ninja, Catch2 v3 (via FetchContent), SHA-256 próprio (sem dependência), Ghidra 12.2 (`~/opt/ghidra_12.2_PUBLIC`), REA.

**Spec:** `docs/superpowers/specs/2026-10-25-dracula-x-pc-design.md`

## Global Constraints

- Linux e Windows (mingw-w64 cross-compile); C++20; CMake + Ninja.
- Licença MIT; núcleos escritos do zero, sem copiar código GPL.
- Nenhum dado do jogo (ISO, arquivos extraídos, imagens, sons) no repositório nem em releases; `data-local/` e `build-local/` no `.gitignore`.
- Disco aceito: apenas T-9527G v1.400 (hash conferido); entradas: cue/bin com setores `MODE1/2352`.
- Nunca `git push` sem pedido explícito.

## Review Focus

- `.cue` com caminho de `.bin` relativo contendo espaços e parênteses (como o do disco real): deve abrir.
- `.bin` truncado ou menor que o declarado: erro claro, sem crash.
- Nome de arquivo ISO9660 com `;1`, minúsculas ou subdiretórios aninhados: extração correta e sem escapar da pasta de destino (`..`).
- Imagem de outro jogo/versão: recusar com mensagem indicando o ID encontrado.
- Pasta de destino já existente com arquivos: não sobrescrever sem `--force`.

---

### Task 1: Esqueleto do repositório

**Files:**
- Create: `CMakeLists.txt`, `LICENSE`, `.gitignore`, `README.md`, `src/saturnfs/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/smoke_test.cpp`

**Interfaces:**
- Produces: alvos CMake `saturnfs` (biblioteca estática), `dxpc-extract` (executável), `tests` (Catch2).

- [ ] **Step 1:** `git init` em `dracula-x-pc/`; `.gitignore` com `build-local/`, `data-local/`, `*.bin`, `*.cue`, `*.iso`.
- [ ] **Step 2:** Escrever `tests/smoke_test.cpp` com `TEST_CASE("toolchain")` que faz `REQUIRE(1 + 1 == 2)`.
- [ ] **Step 3:** Configurar CMake (C++20, Catch2 via FetchContent, `enable_testing`).
- [ ] **Step 4:** Run: `cmake -S . -B build-local -G Ninja && cmake --build build-local && ctest --test-dir build-local` — Expected: 1 teste PASS.
- [ ] **Step 5:** Commit `chore: esqueleto do repositório`.

### Task 2: Leitor de cue e de setores

**Files:**
- Create: `src/saturnfs/cue.hpp/.cpp`, `src/saturnfs/image.hpp/.cpp`
- Test: `tests/cue_test.cpp`, `tests/image_test.cpp`

**Interfaces:**
- Produces:
  - `struct Track { int number; enum class Kind { Data, Audio } kind; std::filesystem::path file; uint32_t start_lba; };`
  - `std::vector<Track> parse_cue(const std::filesystem::path& cue);` (lança `std::runtime_error` se faltar `.bin`)
  - `class DiscImage { public: static DiscImage open(const std::filesystem::path& cue); std::array<uint8_t,2048> read_user_sector(uint32_t lba) const; std::span<const Track> tracks() const; };` (setor MODE1/2352 → 2048 bytes de usuário a partir do offset 16).

- [ ] **Step 1:** Testes: `parse_cue` lê o cue sintético com caminho contendo espaços e parênteses e devolve 2 faixas (Data, Audio); cue apontando `.bin` inexistente lança; `read_user_sector(1)` devolve o padrão escrito no setor 1 do `.bin` sintético; `.bin` truncado lança em `read_user_sector` fora do tamanho.
- [ ] **Step 2:** Run `ctest -R "cue|image"` — Expected: FAIL (não definido).
- [ ] **Step 3:** Implementar `parse_cue` e `DiscImage` (mmap/ifstream com cálculo `lba*2352+16`).
- [ ] **Step 4:** Run — Expected: PASS.
- [ ] **Step 5:** Commit `feat: leitor de cue/bin`.

### Task 3: ISO9660 e verificação do disco

**Files:**
- Create: `src/saturnfs/iso9660.hpp/.cpp`, `src/saturnfs/verify.hpp/.cpp`, `src/saturnfs/sha256.hpp/.cpp`
- Test: `tests/iso9660_test.cpp`, `tests/verify_test.cpp`

**Interfaces:**
- Consumes: `DiscImage::read_user_sector`.
- Produces:
  - `struct FileEntry { std::string path; uint32_t lba; uint32_t size; };`
  - `std::vector<FileEntry> list_files(const DiscImage&);` (percorre diretórios recursivamente; remove `;1`; nomes sempre relativos e sem `..`)
  - `std::vector<uint8_t> read_file(const DiscImage&, const FileEntry&);`
  - `struct SaturnHeader { std::string maker, product_id, version, release_date, title; };`
  - `SaturnHeader read_header(const DiscImage&);` (setor 0, após os 16 bytes de sync/cabeçalho; começa com `SEGA SEGASATURN `)
  - `void require_supported_disc(const SaturnHeader&);` (aceita só `T-9527G` / `V1.400`; mensagem de erro cita o ID encontrado)
  - `std::string sha256_hex(std::span<const uint8_t>);`

- [ ] **Step 1:** Testes com ISO sintético construído em memória: arquivo `A.TXT;1` na raiz e `DIR/B.BIN;1`; `list_files` devolve `A.TXT` e `DIR/B.BIN`; `read_file` devolve os bytes; entrada com nome `..` é ignorada; `read_header` lê `T-9527G`/`V1.400`; `require_supported_disc` lança para `T-0000X` com mensagem contendo `T-0000X`; `sha256_hex("abc")` == `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
- [ ] **Step 2:** Run — Expected: FAIL.
- [ ] **Step 3:** Implementar (descritor primário no setor 16, registros de diretório com tamanho de nome, extensão `;1`).
- [ ] **Step 4:** Run — Expected: PASS.
- [ ] **Step 5:** Commit `feat: ISO9660, cabeçalho Saturn e verificação`.

### Task 4: CLI `dxpc-extract`

**Files:**
- Create: `src/tools/extract_main.cpp`
- Modify: `CMakeLists.txt` (alvo `dxpc-extract`)
- Test: `tests/extract_test.cpp` (função `extract_all(const DiscImage&, const std::filesystem::path& out, bool force)` em `src/saturnfs/extract.hpp/.cpp`)

**Interfaces:**
- Consumes: `list_files`, `read_file`, `require_supported_disc`.
- Produces: `size_t extract_all(const DiscImage&, const std::filesystem::path& out, bool force);` (retorna nº de arquivos; lança se `out` não vazio e `!force`; nunca escreve fora de `out`); CLI: `dxpc-extract <arquivo.cue> <pasta-saida> [--force]`, também grava `manifest.txt` com `caminho lba tamanho sha256`.

- [ ] **Step 1:** Testes: extrair o ISO sintético para pasta temporária cria `A.TXT` e `DIR/B.BIN`; segunda extração sem `force` lança; com `force` sobrescreve; nome malicioso `../x` não escapa.
- [ ] **Step 2:** Run — Expected: FAIL.
- [ ] **Step 3:** Implementar `extract_all` e a `main` (códigos de saída ≠ 0 em erro, mensagem em português).
- [ ] **Step 4:** Run `ctest` completo — Expected: tudo PASS. Depois rodar contra o disco real: `build-local/dxpc-extract "<cue>" data-local/disc` — Expected: lista de arquivos incluindo o executável de boot e `manifest.txt`.
- [ ] **Step 5:** Commit `feat: CLI de extração`.

### Task 5: Análise inicial no Ghidra/REA e mapa de memória

**Files:**
- Create: `docs/analysis/00-boot-e-memoria.md`, `tools/ghidra/README.md`

**Interfaces:**
- Consumes: arquivos extraídos em `data-local/disc` (setor 0 = IP.BIN com endereço de carga e ponto de entrada; executável principal).
- Produces: documento com (a) cabeçalho IP.BIN (endereços de carga/entrada), (b) lista de arquivos do disco com tipo suposto, (c) endereço de carga do executável principal, (d) mapa de memória do Saturn usado (Work RAM-H/L, VDP1/VDP2, SCU, SCSP), (e) procedimento reproduzível para abrir o projeto no Ghidra (arquitetura SH-2 big-endian, endereço-base).

- [ ] **Step 1:** Ler IP.BIN (setores 0–15) e registrar campos de carga/entrada no documento.
- [ ] **Step 2:** Abrir o executável principal com REA (`open_binary`, base de carga do passo 1) e listar procedimentos/strings iniciais; registrar contagem e os primeiros pontos de entrada.
- [ ] **Step 3:** Criar o projeto Ghidra em `data-local/ghidra/` (ignorado pelo git) com SH-2 e o mesmo endereço-base; documentar os passos em `tools/ghidra/README.md`.
- [ ] **Step 4:** Verificação: reabrir pelos passos do README e confirmar que o ponto de entrada desassembla em instruções SH-2 plausíveis.
- [ ] **Step 5:** Commit dos documentos (sem dados do jogo).
