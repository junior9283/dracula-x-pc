# 07 — Boot do jogo real no núcleo (etapa 2) e primeira imagem (etapa 3)

Estado medido com `dxpc-run <cue> --lenient-bios --boot-font <fonte> --frames N` (boot por HLE, sem BIOS real). Tudo aqui é observação do disco do desenvolvedor; nada do jogo é distribuído.

## Marcos atingidos

| Marco | Quadro | Evidência |
|---|---|---|
| M1 — IP.BIN carrega `0.BIN` em `0x06004080` | ~121 | CD block serve a leitura inicial; arquivos idênticos |
| M2 — laço principal com VBlank | ~150 | interrupções 0x40/0x41/0x42/0x47 entregues a cada quadro |
| Abertura `KONAMI.AVI` (VDP1) | 153–296 | captura em 200: quadro do filme desenhado por comandos do VDP1 |
| Logos KCEN e TrueMotion (VDP2) | 308–750 | capturas em 450 e 600 |
| M3 — `TITLE.PRG` em `0x060A5000` e executando | ~880 | 57 321 dos 57 364 bytes idênticos ao arquivo; 1 540 endereços distintos na faixa |
| Tela de título (fundo + logo + copyright) | ~920 | captura em 1500 igual à referência, **sem** "PRESS START BUTTON" |
| Menus, Name Entry e FMV de abertura | 1500–8000 | roteiro de entrada `--pad` (Start@1500, A@1800…3200, Start@3500, A@3900) |
| **Gameplay do estágio 1** (Richter + HUD) | ~8460 | capturas em 9000–9420: andar, subir escada, chicote (A) e pulo (B) respondem |

Instruções distintas executadas no título: ≈ 42 mil (estável de 1 000 a 6 000 quadros).

## Serviços/periféricos descobertos

- **CD block**: DATATRNS em `0x25898000`; comandos 0x42/0x43 (subcabeçalho); relatório periódico (bit PERI); ESEL em 0x30/0x52; sessão 1 em FAD 0; `Play` contínuo (retoma ao liberar o buffer, BFUL).
- **SMPC**: conclusão do comando atrasada (`kCommandCycles`); INTBACK de status + continuação por IREG0 bit 7; o bloco de periférico exige `SR = 0xC0` (PDL+NPE) — com 0x80 o SGL ignora o controle.
- **SH-2**: captura de entrada do FRT (MINIT 0x01800000 / SINIT 0x01000000), DMAC on-chip canais 0/1 (pedido automático), escravo em HLE parte em `[0x06000250]`.
- **Escravo / BIOS**: `SYS_SETSINT` (slot 0x06000310) grava direto em VBR + 4·vetor; o vetor 0x94 cai em `0x06000250`, que é o ponto de entrada do escravo. Com o serviço como no-op, o SSHON relançava o laço obsoleto do filme, o escravo morria e o servidor de streaming do estágio (`[0x06063BE0]`) travava no estado 1 (tela preta). Pilha padrão do escravo: `0x06001000` (IP +0xEC = 0); tratadores do SCU do BIOS em `0x06000900 + 4·vetor`.
- **SCU**: DMA direto e **indireto** (tabela contagem/destino/origem, bit 31 = última). O SGL envia as listas do VDP1 por DMA indireto.
- **Som (provisório)**: o jogo consulta uma tabela de áreas em `0x25A00500` (classes 0x10–0x12 dos `SD/SD_*.CRT`) e o contador do anel de áudio do reprodutor de filmes em `0x25A007A0`; ambos eram stubs até haver 68000 + SCSP.

## Desempenho do interpretador

2500 quadros (boot → menus), Release: 41,3 s → 27,0 s (≈ 60 → 92 quadros/s), sem mudar a saída (quadro 9000 idêntico byte a byte). Ganhos: busca de instrução direto da página de RAM/ROM (`Cpu::fetch16`, invalidada por `Bus::map_generation`), teste `BiosHle::is_trap` antes de chamar o HLE, gravador de endereços executados só com `--coverage`/`--trace` e tabela de opcodes guardada na CPU. LTO não trouxe ganho mensurável. Próximos candidatos: atalho de página também para leituras/escritas de dados e renderizador VDP2 sem divisões por pixel.

## Vídeo

Renderizador de software em `src/video` (VDP2 NBG0–3 em células, VDP1 com sprites/polígonos/linhas). O caractere suplementar do NBG segue `char = (SCN << 10) | pn[9:0]` (1x1, CNSM=0).

## Pendências conhecidas

- Artefatos no estágio: lua/nuvens sobre a torre (prioridade, cálculo de cor ou janelas do VDP2).
- VDP2: já emulados RBG0/RBG1 (parâmetros A/B, tabela de coeficientes, estouro), janela de linha e janela de sprite, validados só com dados sintéticos (o jogo não os liga; ver o censo abaixo). Faltam mosaico, alta resolução/entrelaçado nas camadas de rotação, janela de cálculo de cor, seleção do banco de coeficientes (RDBS/RAMCTL) e line color screen dos coeficientes.
- Temporização do boot do BIOS real: o jogo sai ~3 s antes que no Mednafen (o jingle do BIOS coincide, com atraso constante de 0,143 s); causa não investigada.

## Ferramentas do `dxpc-run`

`--screenshot`, `--layers`, `--dump-video`, `--pad quadro:máscara`, `--poke quadro:endereço=valor`, `--flow-frame`, `--sample-pc`, `--vdp1-log`, `--watch-write`, `--stop-on-write`, `--stop-on-unmapped`, `--history`, `--dump-ram`, `--log-cd`.

## Boot pelo BIOS real (validação local)

Com `dxpc-run <cue> --bios <bios.bin>` o núcleo executa o BIOS de verdade (só para validação na máquina do desenvolvedor; o BIOS
nunca é distribuído e o port usa o HLE). Sequência medida: animação e logo do BIOS, licença do IP.BIN, o BIOS lê o IP (Play com
16 setores) e o `0.BIN` (ReadFile, 153 setores), Init 0x0401 e salto para `0x06004080` no quadro ~682; depois logo Konami, KCEN e o
título por volta do quadro 1500 — o mesmo caminho do HLE, com som. O HLE seguiu idêntico até o gameplay (determinístico).

O que precisou existir no núcleo para isso:
- **DSP do SCU** (`src/saturn/scu_dsp.*`): o BIOS o usa como motor de DMA na abertura. Uma instrução a cada 2 ciclos do SH-2,
  busca adiantada (slot de atraso nos saltos, LPS), ALU de 48 bits, MVI condicional, DMA com os modos de incremento (no B-bus cada
  meia palavra soma o passo), END/ENDI (interrupção 0x45).
- **CD block**: autenticação 0xE0/0xE1 (disco Saturn → tipo 4), Get Copy Error 0x67, Abort File sinaliza EFLS, Reset Selector
  com as flags (partições, condições, conexões), Init com reset por software (HIRQ MPED|EFLS|ECPY|EHST|ESEL), Get File Info com
  tamanho de unidade, intervalo, número de arquivo XA e atributos XA.
- **SMPC**: CKCHG352/320 desliga escravo e som, reinicia o SCU e conclui com NMI; NMIREQ; RESENAB/RESDISA, CDON/CDOFF, SETSMEM;
  INTBACK de status com RTC fixo (1995-01-01), área, estado do relógio/escravo/som/CD e SMEM.
- **SH-2**: bit MASTER do BCR1 lê 1 no escravo (o BIOS usa isso no reset para não limpar a RAM); NMI com nível 16, vetor 11.
- **Backup interno**: bloco 0 com o cabeçalho "BackUpRam Format"; sem ele o jogo pede para inicializar a memória no título.

## Ir direto a outras fases (diagnóstico)

**[obs]** A fase atual fica em `0x0605D750` (16 bits), que é o índice na tabela de estágios do `0.BIN` (`0x06047818`, 24 bytes
por registro). Os índices coincidem com os do SotN de PlayStation: `0x1F` = `STAGE_00` (prólogo) e `0x41` = `STAGE_0X`
(entrada do castelo). O título grava `0x41` no quadro ~1800; o código em `0x0600D9AA` grava `0x1F` no quadro 8120, antes de
carregar o prólogo. Com o roteiro de entrada padrão, `--poke 8121:0x0605D750=<índice>` faz o jogo carregar outra fase (com o
Richter), o que permite capturas e censos de vídeo e de áudio fora do prólogo.

### Censo de vídeo por fase **[obs]**
Para cada índice de fase em `{0x00–0x0E, 0x11–0x14, 0x16–0x2E, 0x35–0x3E, 0x40, 0x42, 0x43}` carregado com `--poke` no quadro 8121 e executado até o quadro 9600, registramos todas as escritas do código do jogo (PC ≥ `0x06000000`, quadro ≥ 8121) nos registradores BGON, RPMD, WCTLA–D, LWTA0/1, CCCTL e SPCTL do VDP2:
- **Nenhuma** fase liga RBG0/RBG1 (BGON bits 4/5 sempre 0), habilita janela de linha (LWTA bit 15) ou usa a janela de cálculo de cor (WCTLD sempre 0). Só dá para afirmar isso para o que as fases fazem até ~1 500 quadros depois de carregadas; chefes e eventos tardios não foram percorridos.
- O índice `0x0E` é `WARP.PRG` (stub) e não carrega por esse atalho: o jogo cai em código inválido, então seus valores de BGON são lixo e foram descartados. Nove execuções não terminaram (índices sem fase e estouro de tempo de 900 s com a máquina carregada): 0x11, 0x25, 0x2B–0x2E, 0x35–0x37.
- As capturas (`sXX_a/b.png`) mostram dezenas de fases desenhadas corretamente (biblioteca, capela, torre do relógio, cavernas, game over…), mas sem a câmera inicial da fase, porque o atalho pula a preparação normal.
