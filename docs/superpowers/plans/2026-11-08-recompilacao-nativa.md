# Plano — o jogo nativo de PC (recompilação real)

Criado em 2026-11-08. **Este é o objetivo central do projeto.** Acompanhamento: `docs/recompilacao/DIARIO.md` (andamento) e
`docs/recompilacao/ERROS.md` (registro de erros). Regras de trabalho: `CLAUDE.md` na raiz.

## 1. Objetivo e decisão do usuário
- **Objetivo:** o *jogo em si* deve rodar como código nativo de PC. Hoje o port é um **emulador dedicado**: o programa original do
  Saturn (SH-2 do jogo e driver de som 68000, lidos do disco do usuário) é **interpretado**. O interpretador **não** conta como
  "100% nativo" (decisão do usuário, 2026-11-08). O que a meta pede: traduzir esse código para código de máquina do PC (x86-64 e
  arm64) a partir do disco do usuário, mantendo o mesmo comportamento.
- **O que continua reimplementado** (é inevitável, mesmo numa recompilação completa): VDP1/VDP2, SCSP, SCU, SMPC, bloco de CD e a
  BIOS por software. "Nativo" aqui significa que o *código do jogo* deixa de ser interpretado.
- **Legal:** o código gerado deriva do jogo, então é gerado **na máquina do usuário** e nunca vai ao repositório nem aos pacotes. O
  repositório só tem o gerador e o runtime.
- O design original (`docs/superpowers/specs/2026-10-25-dracula-x-pc-design.md`) já previa: recompilação estática assistida, com
  interpretador como rede de segurança durante o desenvolvimento. A análise da etapa 1 (`docs/analysis/06-mapa-do-jogo.md`) inverteu a
  ordem (interpretador primeiro); este plano executa a segunda metade.

## 2. Ponto de partida (medido em 2026-11-08, versão v0.2.0)
| Item | Valor |
|---|---|
| Código do jogo | `0.BIN` (312 672 B em `0x06004080`) + 78 overlays de código (79 programas com o `0.BIN`) + 2 de dados |
| Análise estática (Ghidra) | 2 709 funções, 1 119 640 instruções, 41 388 `jsr @Rn`, 1 754 `jmp @Rn` (1 448 parecem tabelas de salto, 306 não) |
| Cobertura estática | piso de 34% dos bytes dos arquivos (não é estimativa do total de código) |
| **Cobertura dinâmica** | **61 912 endereços de instrução distintos** do boot até o gameplay do estágio 1 (9 600 quadros): ~5,5% do achado estaticamente |
| Driver de som 68000 | 40 960 B (`data-local/sdriver68k.bin`), localizado em `0x0603AA7C` |
| Custo por quadro (gameplay do estágio 1) | emulação 4,6 ms + vídeo 4,3 ms = 8,8 ms (limite 16,7 ms). **O vídeo em software já custa tanto quanto a emulação** |
| Execuções por quadro | ~1 milhão de instruções SH-2 (dois SH-2); ~61% eram laços de espera antes da aceleração |
| Testes | 382 casos; prova de identidade bit a bit em `tools/reference/identidade.sh` |
| Não medido | código auto-modificável; alvos reais de `jsr @Rn`/`jmp @Rn`; o que é código e o que é dado nos overlays |

## 3. Arquitetura proposta
```
disco do usuário ─► [gerador local] ─► módulos nativos por programa (0.BIN, cada overlay, driver 68000)
                                  │
 SH-2 do jogo ─► lifter ─► IR ─► emissor (C++ | código de máquina | JIT)        ← escolha em aberto (seção 5)
                                  │
 runtime: tabela endereço→função nativa; chamada/salto indireto consulta a tabela; alvo não traduzido cai no interpretador
          (rede de segurança, medida: meta ≥ 99,9% das instruções executadas em código nativo, depois 100%)
```
- **IR própria**, pequena, com a semântica exata do SH-2 (T, S, Q, M, MAC, slot de atraso, ciclos por instrução).
- **O interpretador atual é o oráculo**: toda tradução é conferida contra ele (testes diferenciais), e ele continua como reserva.
- **Overlays:** cada programa é um módulo; ao carregar um arquivo num endereço, o runtime confere o hash do conteúdo e liga o módulo
  correspondente. Conteúdo diferente do esperado (ou escrita numa página de código) invalida o módulo e cai no interpretador.
- **Temporização:** um bloco traduzido só executa inteiro se o crédito de ciclos da fatia atual comporta os ciclos do bloco; o resto da
  fatia termina no interpretador, instrução a instrução. Isso preserva a entrega de interrupções do modelo atual (que as trata nos
  limites de fatia). O entrelaçamento mestre/escravo por instrução **não** é preservável com blocos: ver risco R1.

## 4. Fases
Tamanhos: **P** (uma sessão), **M** (algumas), **G** (muitas), **GG** (a maior parte do projeto).

### F0 — Levantamento com dados reais (M)
Medir o que a análise estática não conseguiu, instrumentando o interpretador (já existe o gravador de execução):
1. Cobertura dinâmica completa: percorrer todas as fases, chefes, finais, menus e vídeos (roteiros de entrada por fase; o atalho
   `--poke quadro:0x0605D750=<índice>` carrega cada fase). Saída: lista de endereços executados por programa e por fase.
2. **Alvos reais** de cada `jsr @Rn`, `jmp @Rn` e `rts` observados (para resolver os 41 388 + 1 754 indiretos).
3. **Código auto-modificável:** escritas em regiões que depois são executadas; cópias de código entre regiões; o que o BIOS HLE grava.
4. Separar código de dados dentro de cada overlay (pools de literais, tabelas).
5. Quais overlays compartilham o mesmo endereço-base (sobreposição) e se algum par é carregado ao mesmo tempo.
- **Saída:** `docs/analysis/09-cobertura.md`. **Critério:** relatório com números por programa e lista de riscos (auto-modificação, alvos
  dinâmicos). Se houver auto-modificação relevante, a estratégia da seção 3 muda (revisar antes de seguir).

### F1 — IR e lifter do SH-2 (G)
Decodificar as ~140 instruções do SH-2 para a IR, com ciclos, slots de atraso, SR/T/S/Q/M, MAC.L/MAC.W, divisão (DIV0S/DIV0U/DIV1),
acessos desalinhados (exceção de endereço) e áreas de cache. **Critério:** teste diferencial por opcode: para milhares de estados
aleatórios, o resultado da IR (executada por um avaliador de referência) é **idêntico** ao do interpretador (registradores, memória, ciclos).

### F2 — Emissor de referência em C++ e medição (G)
Gerar C++ a partir da IR para um programa (`0.BIN`), compilar e ligar ao runtime. Serve para validar a exatidão e medir velocidade com um
compilador forte, e para descobrir problemas cedo. **Critério:** `0.BIN` traduzido roda o boot até o título com as saídas conferidas
(`identidade.sh`); velocidade medida e registrada. **Não decide a entrega**: ver seção 5.

### F3 — Fluxo de controle e saltos indiretos (G)
Construir blocos e funções; tabelas de salto (`mova`+`jmp`), chamadas indiretas via tabela endereço→função; retornos (`rts` usa PR);
saídas do módulo para o interpretador. Usar os alvos da F0 para semear as entradas. **Critério:** fração das instruções executadas em
código nativo medida por fase (meta ≥ 99,9%), com fallback registrado em log para cada alvo não traduzido.

### F4 — Overlays e código dinâmico (G)
Gerar um módulo por programa (79), ligar por hash ao carregar do disco, invalidar na escrita em página de código. **Critério:** percorrer
todas as 56+ fases com os roteiros da F0, com o fallback medido por programa.

### F5 — Integração com o escalonador, interrupções e o SH-2 escravo (G)
Regra de fatia da seção 3; escravo; MINIT/SINIT; aceleração de laços de espera dentro do código traduzido (um laço de espera vira uma
espera por evento). **Critério:** equivalência de saída nos roteiros de referência (seção 6) e desempenho medido.

### F6 — Driver de som 68000 (M a G)
Mesma ideia para o driver de 40 960 B: lifter 68000 (subconjunto realmente usado, conforme a cobertura da F0), IR, emissor. O SCSP
continua como mixador nativo. **Critério:** áudio idêntico ao atual (correlação 1,0000 por canal, `hle.wav` igual).

### F7 — Entrega ao usuário final (G, depende da decisão da seção 5)
Gerador local integrado ao `--install`, empacotamento Linux/Windows/macOS (Intel e Apple Silicon), CI, documentação, versão `v0.3.0`+.

### Trilha paralela V — vídeo (M a G, independente)
O renderizador em software custa 4,3 ms por quadro no gameplay. Se a recompilação reduzir a emulação, o vídeo vira o limite. Opções:
otimizar o software (SIMD, divisão por faixas em threads) ou mover para GPU (a especificação original previa OpenGL 3.3). Só atacar se
a medição pedir.

## 5. Decisão em aberto — como entregar o código nativo (do usuário)
| Opção | Como | Prós | Contras |
|---|---|---|---|
| **A. C++ gerado + compilador do usuário** | o `--install` gera C++ e chama o compilador do sistema | máxima otimização; mais simples de validar (é o que a F2 já faz) | exige compilador no PC do usuário (raro no Windows e no macOS) |
| **B. Tradutor dinâmico (JIT) embutido** | o executável traduz blocos para x86-64/arm64 na hora e guarda em cache | nada a instalar; vale para Windows, Linux e macOS arm64 | é código nativo gerado *em tempo de execução*, não recompilação estática; memória executável (W^X, `MAP_JIT` no macOS) |
| **C. Backend próprio, estático** | o `--install` emite código de máquina x86-64/arm64 direto num arquivo e o runtime o carrega | estático e sem compilador externo | maior esforço (dois emissores, relocação, depuração) |
**Recomendação:** F0 a F2 são iguais nas três opções, então começam já. Decidir A, B ou C só ao fim da F2, com a medição real de
velocidade e do tamanho do trabalho. Uma saída provável: C++ gerado (A) para desenvolvimento e Linux, mais B ou C para entregar a
Windows e macOS.

## 6. Verificação
1. **Diferencial por instrução** (F1): IR contra interpretador, milhares de estados aleatórios por opcode.
2. **Rastro em código real** (F2/F3): reproduzir o rastro de execução gravado e comparar registradores nos limites de bloco.
3. **De ponta a ponta** (F2 em diante): `tools/reference/identidade.sh <dxpc-run> <pasta>` com o binário de referência e com o novo; imagem,
   áudio e estado completo da máquina devem ser idênticos. Se o entrelaçamento mestre/escravo impedir a igualdade bit a bit (R1), adotar
   equivalência de **saída por quadro** nos roteiros (imagem e áudio idênticos nos mesmos quadros), documentando cada diferença.
4. Referências de hoje (código atual, `v0.2.0` + commits seguintes): HLE 4000 → PNG `4879f4761d0822ac47cd23561917f93a22ba12fe`; PNG do quadro
   9600 `633e2178ece22279db2b4418fab8e6b75aec3de1`; WAV 9600 quadros `e8b97fda66b4a51d880ec350c539738165595f83`; estado 9600
   `73d2047239fe241f12a74194df2b4b1c96ccf76f`; BIOS real 2400: PNG `2b9fdc706aa243c050857efdac0c6ec8a667f0f7`, WAV
   `413da6d24d7b28a1dd0761797720d423e10bae8e`, estado `ec9499e4de9f042fd21a179dfdda444250edcf3b`. Regenere com o script se o núcleo mudar de propósito.

## 7. Riscos
| # | Risco | Tratamento |
|---|---|---|
| R1 | O interpretador entrelaça mestre e escravo **por instrução** e entrega interrupções em limites de fatia; blocos traduzidos não preservam o entrelaçamento por instrução | regra de fatia (seção 3); medir; se preciso, equivalência por quadro; o jogo tem de tolerar variação de temporização, como no hardware real |
| R2 | Código auto-modificável ou alvos dinâmicos em quantidade | F0 mede antes de qualquer decisão; módulos invalidam por escrita; fallback no interpretador |
| R3 | A cobertura dinâmica nunca é completa (chefes, finais) | trilha de cobertura contínua; fallback registrado; módulos gerados também por varredura estática dos arquivos |
| R4 | Escolha de entrega (seção 5) inviabiliza macOS/Windows sem compilador | decisão na F2 com dados |
| R5 | Vídeo em software vira o gargalo | trilha V |
| R6 | Tamanho: é a maior parte do projeto | fases com critérios mensuráveis; cada fase termina com testes e hashes |

## 8. Como retomar numa nova sessão
1. Leia `CLAUDE.md`, este plano, `docs/recompilacao/DIARIO.md` (última entrada) e `docs/recompilacao/ERROS.md` (abertos).
2. Build de trabalho: `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`; testes: `build-release/tests/tests`.
3. Dados locais (ignorados pelo git): `data-local/` (BIOS do usuário `bios-local.bin`, `boot-font.bin`, `sdriver68k.bin`, `disc/` extraído). O disco fica em
   `../CASTLEVANIA SON/*/*.cue`. A fonte de boot vem de `tools/extract_boot_font.py` (ver o cabeçalho do script).
4. Medições: `dxpc-run <cue> --lenient-bios --boot-font data-local/boot-font.bin --frames 9600 $(cat tools/reference/pad-gameplay.txt) --frame-log f.csv`
   e `python3 tools/analysis/frame_times.py f.csv 8400 9600`; cobertura: `--coverage`; trocar de fase: `--poke 8121:0x0605D750=<índice>`.
5. Estado público: repositório `junior9283/dracula-x-pc`, página https://junior9283.github.io/dracula-x-pc/, releases v0.1.0 e v0.2.0 (Linux e Windows; macOS em
   andamento pelo GitHub Actions, ver o diário).
6. Subagentes: o usuário autoriza vários em paralelo quando as tarefas são independentes e não se atrapalham (cópias isoladas, builds em `/tmp`).
