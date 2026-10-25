# Etapa 2 — Núcleo: interpretador SH-2, barramento, periféricos de sistema e boot

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Um executável de linha de comando (`dxpc-run`) que inicia o jogo sem BIOS real, executa o código SH-2 original (IP.BIN → `0.BIN` → `TITLE.PRG`) por interpretação, atende os periféricos de sistema que o jogo exige e roda o laço principal com interrupções de VBlank — sem vídeo ainda.

**Architecture:** Bibliotecas `sh2` (CPU + periféricos internos), `saturn` (barramento, SCU, SMPC, CD block, tempo de vídeo, BIOS HLE, `Machine`) e `dxpc-run`. O jogo é servido a partir de uma `SectorSource` (nesta etapa lida do disco do desenvolvedor; na etapa 6 passa a vir do pacote local). Todo comportamento de hardware é coberto por testes com programas SH-2 escritos à mão; a validação com o jogo real é por marcos (M1–M3).

**Tech Stack:** C++20, CMake/Ninja, Catch2 (já no projeto). Nenhuma dependência nova no produto. **Referência local de desenvolvimento:** Mednafen 1.32.1 + BIOS do próprio usuário (ver abaixo).

**Spec:** `docs/superpowers/specs/2026-10-25-dracula-x-pc-design.md` (seção "Revisão após a etapa 1") · **Fatos de partida:** `docs/analysis/00`–`06`

## Global Constraints

- C++20, Linux e Windows; licença MIT; núcleos escritos do zero (emuladores só como referência de comportamento observado).
- Nenhum dado do jogo no repositório; `data-local/` ignorado pelo git; nenhum BIOS real é usado nem distribuído (boot por HLE).
- Estratégia: **interpretador SH-2 primeiro**, sem ISO e sem emulador externo em runtime (nesta etapa a fonte de setores é o disco do desenvolvedor, via `SectorSource`).
- Dois SH-2 (mestre/escravo) a 28,63636 MHz; o escravo só executa após o comando SMPC `SSHON`.
- **Mednafen e a BIOS do usuário são só referência local de comportamento:** nenhum código, dado, estado ou captura deles entra no repositório; a BIOS nunca é copiada (só linkada em `data-local/`); o produto final não depende de BIOS (HLE).
- Nunca `git push` sem pedido explícito.

## Referência: Mednafen (já preparada)

- `tools/reference/mednafen_record.sh <cue> <pasta> <duração> [instantes…]` roda o Mednafen em X virtual (sem janela na sessão do usuário, home isolado) e produz `audio.wav` (48 kHz) e quadros PNG 352×240 por instante. Notas aprendidas: o SDL2 usa o Wayland da sessão se `WAYLAND_DISPLAY` existir (por isso o script força X11 virtual); o OpenGL por software é lento (usa-se `-video.driver sdl`); o `.mov` do `-qtrecord` guarda quadros com largura ×30; **injeção de teclas (F5/F9) não funcionou** — por isso save states e depurador do Mednafen **não** são usados.
- **Linha do tempo real medida (BIOS japonesa v1.01 + este disco):** 0–13 s abertura da BIOS (cacos de vidro, logo SEGA SATURN, “Produced by or under license from SEGA”); ≈20–26 s o jogo mostra o logo da Konami (`KONAMI.AVI`), depois “Konami Computer Entertainment Nagoya” e o aviso da TrueMotion (~37–40 s). Referência para a ordem de eventos dos marcos M2/M3 (o nosso boot por HLE dispensa a abertura da BIOS).
- **BIOS real como teste do núcleo (Task 13):** rodar a BIOS do usuário no nosso núcleo é a validação mais forte de CPU/SCU/SMPC/CD block; só em desenvolvimento.

## Fatos de partida (docs/analysis)

- IP.BIN: carregado em `0x06002000` (0x1800 B), entrada em `0x06002100`; lê ponteiros de função do BIOS em `0x06000260`, `0x06000300`, `0x06000344` e chama-os; lê `0x06000248`; escreve em VDP2 VRAM (`0x25E0xxxx`) e Color RAM.
- `0.BIN`: `0x06004080–0x060505DF`, entrada em `0x06004080` (salva r8/r14/PR, chama init, laço `jsr @r8`).
- Carga de arquivos: `0x0600654C(r4=nome, r5=destino)`. CD block acessado por 15 literais em `0.BIN`. Overlay de cena (`TITLE.PRG`) em `0x060A5000`, de fase em `0x060DC000`.
- Periféricos usados por literais: SMPC 65 (em `0.BIN`), SCU DMA 6, SCSP 11, VDP1/VDP2 (a etapa 3), `MINIT` 1, SCU DSP 0.

## Review Focus

- **Instrução de desvio dentro de um delay slot** → exceção de instrução ilegal no slot (vetor 6); não executa o desvio.
- **Interrupção que chega durante um delay slot** só é atendida depois que o desvio do slot termina; o PC salvo é o alvo correto.
- **Acesso desalinhado** (`mov.w`/`mov.l` com endereço ímpar/não múltiplo de 4) → exceção de erro de endereço (vetor 9 para CPU, 10 para DMA), não leitura silenciosa.
- **Divisão por `DIV1`** (sequências 32/16 e 64/32): quociente/resto corretos, inclusive divisão por zero e overflow, comparados com a divisão inteira de referência.
- **Escrita em ROM/região somente leitura** é ignorada; **acesso de 8 bits a registrador de 16/32 bits** (VDP2, SCU, CD block) segue a regra documentada de cada dispositivo (teste por dispositivo); **alias de cache-through** (`0x06…` ≡ `0x26…`) acessa a mesma RAM.

---

### Task 1: Barramento e mapa de memória

**Files:**
- Create: `src/saturn/bus.hpp/.cpp`, `src/saturn/CMakeLists.txt`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt` (link `saturn`)
- Test: `tests/bus_test.cpp`

**Interfaces:**
- Produces:
  - `class IoDevice { virtual ~IoDevice(); virtual uint32_t read(uint32_t offset, int bytes) = 0; virtual void write(uint32_t offset, int bytes, uint32_t value) = 0; };`
  - `class Bus` com `uint8_t read8(uint32_t)`, `uint16_t read16(uint32_t)`, `uint32_t read32(uint32_t)` e `write8/16/32(uint32_t, valor)` (big-endian); `void map_ram(uint32_t base, uint32_t size, uint32_t mirror_span)`; `void map_rom(uint32_t base, std::span<const uint8_t> data)`; `void map_io(uint32_t base, uint32_t size, IoDevice&)`; `std::span<uint8_t> ram_at(uint32_t address)` (para DMA/carregadores).
  - `void map_standard_memory(Bus&)`: Work RAM-L 1 MB em `0x00200000`, Work RAM-H 1 MB em `0x06000000` repetida até `0x07FFFFFF`.
  - Regra de endereço: bits 31–29 = `000` ou `001` (cache-through) → `addr & 0x1FFFFFFF`; não mapeado: leitura devolve 0 e registra uma vez.

- [ ] **Step 1:** Testes: `write32(0x06000000, 0x11223344)` → `read8(0x06000000)==0x11`, `read16(0x06000002)==0x3344`; `read32(0x26000000)` vê o mesmo valor (alias); `write32(0x06100000,…)` aparece em `0x06000000` (espelho de 1 MB); `map_rom` ignora escrita; `map_io` encaminha `(offset, bytes, valor)` ao dispositivo com `bytes` = 1/2/4; leitura não mapeada = 0.
- [ ] **Step 2:** Run `ctest --test-dir build-local` — Expected: FAIL.
- [ ] **Step 3:** Implementar `Bus` (tabela de regiões por página de 64 KiB, resolvida na construção do mapa).
- [ ] **Step 4:** Run — Expected: PASS. **Step 5:** Commit `feat: barramento e mapa de memória do Saturn`.

### Task 2: Núcleo SH-2 A — registradores, busca/decodificação e transferência de dados

**Files:**
- Create: `src/sh2/cpu.hpp/.cpp`, `src/sh2/ops_data.cpp`, `src/sh2/CMakeLists.txt`, `tests/sh2_harness.hpp`
- Test: `tests/sh2_data_test.cpp`

**Interfaces:**
- Consumes: `Bus`.
- Produces:
  - `struct Sh2State { uint32_t r[16], pc, pr, gbr, vbr, mach, macl, sr; bool in_delay_slot; uint32_t delay_target; };` (`sr`: T=bit0, S=bit1, I=bits 4–7, Q=bit8, M=bit9)
  - `class Cpu { Cpu(saturn::Bus&, int id); void reset(uint32_t pc, uint32_t sp); uint32_t step(); /* devolve ciclos */ Sh2State& state(); }`
  - Harness de teste: `assemble(std::initializer_list<uint16_t>)` grava no endereço `0x06000000` e executa `n` passos.

- [ ] **Step 1:** Testes por grupo, com valores esperados do manual SH-1/SH-2: `mov #imm,Rn` (extensão de sinal), `mov Rm,Rn`, `mov.b/w/l` com `@Rm`, `@Rm+`, `@-Rn`, `@(disp,Rm)`, `@(R0,Rm)`, `@(disp,GBR)`, `@(disp,PC)` (alinhamento `(PC&~3)`), `mova`, `movt`, `swap.b/w`, `xtrct`, `ldc/stc/lds/sts` (SR, GBR, VBR, MACH, MACL, PR) — incluindo extensão de sinal de `mov.b/w` para registrador.
- [ ] **Step 2:** Run — FAIL; **Step 3:** implementar a decodificação por tabela de 65 536 entradas e as operações de dados; **Step 4:** Run — PASS; **Step 5:** Commit.

### Task 3: Núcleo SH-2 B — aritmética, lógica, deslocamento

**Files:** Create `src/sh2/ops_alu.cpp`; Test `tests/sh2_alu_test.cpp`

**Interfaces:** Consumes `Cpu`/`Sh2State`. Produces nenhuma API nova (instruções passam a executar).

- [ ] **Step 1:** Testes: `add/addc/addv`, `sub/subc/subv`, `neg/negc`, `cmp/eq|hs|ge|hi|gt|pz|pl|pstr|str`, `and/or/xor/not/tst` (e as formas `#imm,R0` e `@(R0,GBR)`), `shll/shlr/shal/shar/rotl/rotr/rotcl/rotcr` e `shll2/8/16`, `extu.b/w`, `exts.b/w`, `dt`, `tas.b`, `clrt/sett`, `clrmac` — checando T/carry/overflow nos casos de borda (`0x80000000`, `0xFFFFFFFF`).
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 4: Núcleo SH-2 C — desvios, delay slot, exceções e interrupções

**Files:** Create `src/sh2/ops_branch.cpp`, `src/sh2/exceptions.cpp`; Test `tests/sh2_branch_test.cpp`

**Interfaces:**
- Produces: `void Cpu::raise_interrupt(int level, int vector)` (pendente até `level > SR.I` e fora de delay slot); `bool Cpu::sleeping() const`.
- Exceções: reset, instrução ilegal (4), instrução ilegal no slot (6), erro de endereço CPU (9), `trapa` (vetor = imediato), NMI (11), IRQ. Entrada de exceção: empilha SR e PC, `SR.I` ← nível (interrupções), PC ← `@(VBR + vetor*4)`.

- [ ] **Step 1:** Testes (inclui os itens do **Review Focus**): `bra/bsr/braf/bsrf/jmp/jsr/rts/rte` com delay slot; `bt/bf` e `bt/s`/`bf/s`; desvio dentro de delay slot → vetor 6; interrupção durante delay slot só depois do desvio; `rte` restaura SR/PC (e executa o slot); acesso desalinhado `mov.l @R1` com R1 ímpar → vetor 9; `trapa #n`; `sleep` para até uma interrupção; máscara `SR.I` bloqueia nível ≤ I.
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 5: Núcleo SH-2 D — multiplicação, divisão e periféricos internos

**Files:** Create `src/sh2/ops_mul.cpp`, `src/sh2/onchip.hpp/.cpp`; Test `tests/sh2_mul_test.cpp`, `tests/sh2_onchip_test.cpp`

**Interfaces:**
- Produces: `class OnChip : public saturn::IoDevice` (registradores em `0xFFFFFE00–0xFFFFFFFF`) contendo **DIVU** (`DVSR/DVDNT/DVCR`, `0xFFFFFF00…`), **FRT** (contador livre, `TCR/TOCR/FTCSR`) e **INTC** (`IPRA/IPRB/VCRx`); `Cpu` encaminha acessos `>= 0xFFFFFE00` ao `OnChip`.
- Instruções: `mul.l`, `muls.w`, `mulu.w`, `dmuls.l`, `dmulu.l`, `mac.w`, `mac.l` (com saturação por `S`), `div0s/div0u/div1`.

- [ ] **Step 1:** Testes: `dmulu.l`/`dmuls.l` em `MACH:MACL` contra o produto de 64 bits de referência; `mac.l` com e sem `S`; **sequência `DIV1` completa 32/16 e 64/32** comparada com `/` e `%` de referência em vetores incluindo 0, negativos, `INT_MIN` e divisão por zero; DIVU: escrever `DVDNT` dispara a divisão e `DVDNT/DVDNTH/DVDNTL` leem quociente/resto, `OVF` em estouro; FRT avança com os ciclos do CPU.
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 6: Gravador de traço e cobertura

**Files:** Create `src/sh2/trace.hpp/.cpp`; Test `tests/trace_test.cpp`

**Interfaces:**
- Produces: `class ExecutionRecorder { void record(uint32_t pc); bool executed(uint32_t pc) const; size_t count() const; void save(const std::filesystem::path&) const; static ExecutionRecorder load(const std::filesystem::path&); }` e `void Cpu::set_recorder(ExecutionRecorder*)` (um gancho por instrução executada, desligado por padrão).

- [ ] **Step 1:** Testes: programa de 5 instruções (com um desvio) executado → `executed()` verdadeiro só nos endereços percorridos; `save`/`load` preserva o conjunto; recorder nulo não custa (sem ponteiro, sem chamada).
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit. (Esse arquivo alimenta a recompilação AOT guiada por traço, etapa posterior.)

### Task 7: SCU, tempo de vídeo e interrupções do sistema

**Files:** Create `src/saturn/scu.hpp/.cpp`, `src/saturn/video_timing.hpp/.cpp`; Test `tests/scu_test.cpp`

**Interfaces:**
- Consumes: `Cpu::raise_interrupt`, `Bus`.
- Produces: `class Scu : public IoDevice` (registradores `0x25FE0000…`): `IMS` (máscara), `IST` (pendentes), `ASR/DMA nível 0–2` básico (cópia de memória), timers 0/1; `void Scu::raise(Scu::Interrupt)` com vetores/níveis padrão (`VBlankIn 0x40/F`, `VBlankOut 0x41/E`, `HBlankIn 0x42/D`, `Timer0 0x43/C`, `Timer1 0x44/B`, `DspEnd 0x45/A`, `SoundRequest 0x46/9`, `SystemManager 0x47/8`, `Pad 0x48/8`, `Dma2 0x49/6`, `Dma1 0x4A/6`, `Dma0 0x4B/5`, `DmaIllegal 0x4C/3`, `SpriteEnd 0x4D/2`).
- `class VideoTiming { VideoTiming(Scu&); void advance(uint64_t cycles); uint16_t tvstat() const; uint64_t frame() const; }`: 262 linhas NTSC, `VBlankIn` na linha 224, `VBlankOut` na 0, `HBlankIn` por linha; `TVSTAT` (`0x25F80004`) reflete VBLANK/HBLANK.

- [ ] **Step 1:** Testes: interrupção mascarada por `IMS` fica pendente em `IST` e é entregue ao desmascarar; `VideoTiming` gera exatamente 1 `VBlankIn` por quadro e 262 `HBlankIn`; `TVSTAT` muda no tempo certo; DMA nível 0 copia N bytes de A para B.
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 8: SMPC

**Files:** Create `src/saturn/smpc.hpp/.cpp`; Test `tests/smpc_test.cpp`

**Interfaces:**
- Produces: `class Smpc : public IoDevice` (`0x00100000…`: `IREG0–6`, `COMREG`, `OREG0–31`, `SR`, `SF`); `COMREG` dispara o comando; `SF` volta a 0 quando concluído e gera a interrupção `SystemManager`. Comandos: `SSHON (0x02)`/`SSHOFF (0x03)` (callback `on_slave_power(bool)`), `SNDON/SNDOFF (0x06/0x07)`, `CKCHG320/352 (0x0E/0x0F)`, `INTBACK (0x10)` (status + um periférico “nenhum/pad digital” configurável por `set_pad(uint16_t)`), `SETTIME (0x16)`, `NMIREQ`, `RESENAB/RESDISA`. Comando desconhecido: registra e conclui.

- [ ] **Step 1:** Testes: escrever `COMREG=0x02` aciona `on_slave_power(true)` e zera `SF`; `INTBACK` entrega o formato de `OREG` documentado (status de 1 B + 2 B de pad com o valor de `set_pad`); acesso de 8 bits em offsets ímpares (a regra do SMPC: só bytes ímpares) — leitura/escrita em offset par é ignorada/0.
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 9: CD block

**Files:** Create `src/saturn/cdblock.hpp/.cpp`, `src/saturn/sector_source.hpp`; Test `tests/cdblock_test.cpp`

**Interfaces:**
- Produces: `class SectorSource { virtual ~SectorSource(); virtual bool read_user_sector(uint32_t lba, std::span<uint8_t, 2048> out) = 0; virtual uint32_t sector_count() const = 0; };` e `class DiscSectorSource : public SectorSource` (sobre `saturnfs::DiscImage`).
- `class CdBlock : public IoDevice` (`0x25890000…`: `HIRQ`, `HIRQ mask`, `CR1–CR4`, e a porta de dados em `0x25818000`) com FAD = LBA + 150. Conjunto de comandos **observado nos acessos de IP.BIN/`0.BIN`** (registrado pelo emulador no primeiro passe): grupos de status (`0x00–0x06`), reprodução/posição (`0x10–0x11`), conexão e filtros (`0x30–0x48`), buffer/setor (`0x50–0x5C`), sistema de arquivos (`0x60–0x67`: `ChangeDirectory`, `ReadDirectory`, `GetFileInfo`, `ReadFile`). Comando fora do conjunto: registra, devolve erro no `CR1` e sinaliza `HIRQ.CMOK`.
- Os nomes de arquivo e diretórios vêm do ISO9660 de `SectorSource` (reutiliza `saturnfs::list_files` por meio de uma `DiscImage`; sem ISO real, a etapa 6 fornece o mesmo contrato pelo pacote).

- [ ] **Step 1:** Testes com `SectorSource` sintético (arquivos `A.TXT`, `DIR/B.BIN`): `GetStatus` devolve estado “pausa/sem disco” coerente; `ChangeDirectory`+`ReadDirectory` listam os arquivos; `GetFileInfo` devolve LBA e tamanho; `ReadFile` + leitura da porta de dados devolve exatamente os bytes do arquivo (incl. mais de um setor); `HIRQ` sinaliza `CMOK`/`DRDY`/`EFLS` nos momentos certos; leitura de 8 bits da porta de dados (largura documentada).
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 10: BIOS HLE, `Machine` e carga de IP.BIN

**Files:** Create `src/saturn/bios_hle.hpp/.cpp`, `src/saturn/machine.hpp/.cpp`, `src/tools/run_main.cpp`; Test `tests/machine_test.cpp`
**Modify:** `CMakeLists.txt` (alvo `dxpc-run`)

**Interfaces:**
- Produces:
  - `class Machine { Machine(std::unique_ptr<SectorSource>); void boot(); /* HLE: carrega IP.BIN de LBA 0–15 em 0x06002000 e salta para 0x06002100 */ void run_cycles(uint64_t); Cpu& master(); Cpu& slave(); Bus& bus(); ExecutionRecorder& recorder(); uint64_t frame() const; }`
  - HLE do BIOS: preenche a tabela de ponteiros em `0x06000200–0x06000350` com endereços de uma **região de gatilhos** na área de BIOS; quando o `pc` cai num gatilho, executa o serviço nativo e emula `rts`. Serviços implementados **por demanda** (cada chamada desconhecida é registrada com endereço/argumentos e para a execução com mensagem clara): `0x06000300` e `0x06000344` (ajuste de vetores/máscara de interrupção do SCU, ver IP.BIN), variáveis de sistema em `0x06000240–0x0600026F`.
  - Agendador: alterna mestre e escravo em fatias de 64 ciclos; o escravo só roda após `SSHON`; `VideoTiming::advance` a cada fatia.
  - `dxpc-run <cue> [--frames N] [--trace arquivo]`: executa N quadros e imprime PCs de saltos para endereços desconhecidos/serviços BIOS chamados.

- [ ] **Step 1:** Testes com SectorSource sintético: `boot()` coloca os 0x1800 bytes do IP em `0x06002000` e `master().state().pc == 0x06002100`; chamada ao gatilho do serviço de vetor escreve o handler na tabela de vetores a partir de `VBR` e devolve ao chamador; serviço desconhecido interrompe com erro descritivo; o escravo não executa antes de `SSHON`.
- [ ] **Step 2–5:** FAIL → implementar → PASS → Commit.

### Task 11: Marcos com o jogo real

**Files:** Create `docs/analysis/07-boot-real.md`, `tests/boot_milestones_test.cpp` (**só roda se `DXPC_CUE` estiver definido**; caso contrário é pulado com `SKIP`)

- [ ] **Step 1 (M1):** Rodar `dxpc-run <cue> --frames 5`; iterar implementando os serviços/registradores que o IP.BIN usar até ele pedir `0.BIN` ao CD block e transferi-lo para `0x06004080`. Verificar: os 312 672 bytes em RAM são idênticos ao arquivo do disco (comparação por SHA-256). Registrar cada serviço BIOS/comando CD descoberto em `07-boot-real.md`.
- [ ] **Step 2 (M2):** Chegar à entrada de `0.BIN` (`pc == 0x06004080`) e ao laço `jsr @r8`; rodar 300 quadros sem exceção não tratada; contar interrupções VBlankIn entregues (= 300).
- [ ] **Step 3 (M3):** Observar `0.BIN` pedir `LOGO.PRG`/`TITLE.PRG`: verificar que `TITLE.PRG` está em `0x060A5000` (bytes idênticos) e que o código dele executa (`ExecutionRecorder` mostra endereços ≥ `0x060A5000`). Se o jogo esperar por VDP1/VDP2 (`TVSTAT`, fim de desenho), registrar o registrador esperado e usar um stub mínimo que apenas satisfaça a espera (o vídeo de verdade é a etapa 3).
- [ ] **Step 4:** Documentar em `07-boot-real.md` os marcos atingidos, os serviços descobertos, a lista de periféricos ainda sem implementação e o traço de cobertura (nº de endereços distintos executados). Commit.

### Task 12: Desempenho de referência

**Files:** Create `tests/bench/sh2_bench.cpp` (alvo `dxpc-bench`, não roda na suíte)

- [ ] **Step 1:** Programa SH-2 sintético em laço (ALU + acessos à RAM) executado por 200 milhões de ciclos; reportar MIPS em `-O2` e em `-O3`.
- [ ] **Step 2:** Meta: **≥ 100 MIPS** neste computador (o jogo exige ~50 MIPS somando os dois SH-2 a 28,6 MHz, ≈ 1 CPI). Se ficar abaixo, registrar o gargalo medido e a otimização escolhida (cache de blocos decodificados, tabela de despacho) antes de seguir; registrar o resultado em `docs/analysis/07-boot-real.md`. Commit.

### Task 13: Validação com a BIOS real do usuário (somente desenvolvimento)

**Files:** Create `docs/analysis/08-validacao-bios.md`, opção `--bios <arquivo>` em `src/tools/run_main.cpp`, `src/saturn/machine.cpp` (modo BIOS), `tests/bios_boot_test.cpp` (**só roda com `DXPC_BIOS` e `DXPC_CUE` definidos; senão `SKIP`**)

**Interfaces:**
- Consumes: `Machine`, `SectorSource`. Produces: `Machine::boot_with_bios(std::span<const uint8_t> rom)` (mapeia a ROM em `0x00000000` como somente leitura, reseta o mestre pelos vetores de reset da ROM, escravo parado) e `uint32_t Machine::run_until_pc(uint32_t pc, uint64_t max_cycles)` (devolve ciclos gastos ou lança por tempo esgotado).

- [ ] **Step 1:** Rodar a BIOS real no núcleo até a primeira fase de vídeo/SMPC/CD; registrar o primeiro acesso a registrador ou comando não implementado (periférico, instrução ou CD block) e implementá-lo com teste de unidade; repetir até a BIOS terminar a inicialização e **transferir o controle ao IP.BIN** (`pc == 0x06002100`, após o boot do disco, sem a animação gráfica, que depende do VDP).
- [ ] **Step 2:** Nesse instante, comparar com o HLE: despejar `0x06000200–0x060003FF` (tabela de ponteiros do BIOS e variáveis de sistema) e as `0x06002000–0x060037FF` (IP.BIN carregado). Para cada divergência em relação à inicialização do HLE (Task 10), ajustar o HLE; o IP.BIN carregado deve ser idêntico ao do disco.
- [ ] **Step 3:** Registrar em `08-validacao-bios.md` os periféricos/instruções que a BIOS real exigiu, os valores reais das variáveis de sistema que o IP.BIN lê (`0x06000248`, `0x06000260`) e o que mudou no HLE. Não copiar trechos da ROM para o repositório. Commit.
