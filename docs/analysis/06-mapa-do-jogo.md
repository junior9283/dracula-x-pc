# Mapa consolidado do jogo e recomendação para a etapa 2 (etapa 1, tarefa 7)

Legenda: **[obs]** observado · **[inf]** inferido. Reproduzir: `tools/ghidra/census_all.sh` (≈25 min; precisa de `data-local/disc` e `data-local/overlay-list.txt` gerado a partir de `dxpc-overlays`) e `python3 -I tools/analysis/census_summary.py data-local/ghidra/census.txt`. Visão geral dos formatos: `00`–`05` nesta pasta.

## 1. Resumo do que se sabe
- **Disco:** 436 arquivos; `0.BIN` (312 672 B) em `0x06004080`; 78 overlays de código com base conhecida e 2 de dados (`01-overlays.md`).
- **Memória [obs]:** `0x06004080–0x060505DF` núcleo; `0x06066000` GAME/MOVIE; `0x060A5000` cenas e personagens; `0x060CF000` familiares; `0x060D6000` submenu; `0x060DC000` fases/chefes. MAP/FON em Work RAM-L (`0x00252000…`); CHR direto na VDP1 VRAM.
- **Carga:** o jogo carrega por **nome de arquivo** (`0x0600654C(nome, destino)`) e abre o diretório `SD` por nome (`02`, `05`): a camada de CD do port precisa servir esses acessos a partir do pacote de dados local (sem ISO).
- **Música:** 49 faixas IMA ADPCM estéreo decodificadas e validadas (`02`); tabela fase→faixa para 24 faixas, o resto por log em execução (`03`).
- **Som:** driver do 68000 localizado em `0x0603AA7C`; emular 68000+SCSP e rodá-lo (`05`).
- **Vídeo:** 4 AVIs convertíveis por ffmpeg (`05`).

## 2. Censo de código (Ghidra, 79 programas) [obs]
| Programa | funcoes | instrucoes | jsr_indireto | jmp_indireto | tabelas_de_salto |
|---|---|---|---|---|---|
| `0.BIN` | 713 | 72895 | 2148 | 55 | 42 |
| `ALUCARD.PRG` | 155 | 54519 | 1626 | 87 | 85 |
| `BOSS_02.PRG` | 11 | 7392 | 358 | 11 | 11 |
| `BOSS_04.PRG` | 83 | 25110 | 767 | 34 | 34 |
| `BOSS_06.PRG` | 19 | 10000 | 374 | 56 | 11 |
| `BOSS_07.PRG` | 68 | 34880 | 1025 | 55 | 55 |
| `BOSS_08.PRG` | 14 | 6875 | 373 | 12 | 12 |
| `BOSS_09.PRG` | 31 | 16456 | 590 | 19 | 19 |
| `BOSS_10.PRG` | 35 | 30163 | 845 | 104 | 28 |
| `BOSS_11.PRG` | 12 | 9630 | 434 | 41 | 22 |
| `BOSS_12.PRG` | 11 | 4107 | 217 | 8 | 8 |
| `BOSS_13.PRG` | 11 | 9252 | 500 | 13 | 13 |
| `BOSS_R02.PRG` | 17 | 6991 | 355 | 12 | 12 |
| `BOSS_R04.PRG` | 11 | 4738 | 237 | 8 | 8 |
| `BOSS_R06.PRG` | 16 | 6236 | 259 | 6 | 6 |
| `BOSS_R08.PRG` | 21 | 9042 | 325 | 12 | 12 |
| `BOSS_R09.PRG` | 83 | 25435 | 752 | 36 | 36 |
| `BOSS_R10.PRG` | 12 | 5475 | 206 | 7 | 7 |
| `BOSS_R11.PRG` | 13 | 11091 | 659 | 41 | 21 |
| `BOSS_R12.PRG` | 18 | 10245 | 499 | 18 | 16 |
| `BOSS_R13.PRG` | 26 | 9625 | 436 | 38 | 17 |
| `BOSS_R14.PRG` | 5 | 9536 | 420 | 20 | 20 |
| `ENDING.PRG` | 13 | 4296 | 141 | 14 | 14 |
| `GAME.PRG` | 51 | 12962 | 342 | 12 | 10 |
| `LIBRARY.PRG` | 17 | 14404 | 438 | 7 | 7 |
| `LOGO.PRG` | 0 | 0 | 0 | 0 | 0 |
| `MARIA.PRG` | 109 | 48671 | 1572 | 70 | 70 |
| `MOVIE.PRG` | 150 | 19891 | 482 | 2 | 2 |
| `OPEN.PRG` | 2 | 459 | 29 | 1 | 1 |
| `RICHTER.PRG` | 93 | 44596 | 1471 | 65 | 65 |
| `RICHTER2.PRG` | 93 | 44565 | 1469 | 64 | 64 |
| `RSTAGE01.PRG` | 35 | 22865 | 908 | 28 | 24 |
| `RSTAGE02.PRG` | 12 | 17659 | 861 | 38 | 30 |
| `RSTAGE03.PRG` | 13 | 7629 | 228 | 4 | 4 |
| `RSTAGE04.PRG` | 17 | 11817 | 463 | 9 | 9 |
| `RSTAGE05.PRG` | 14 | 9940 | 382 | 11 | 11 |
| `RSTAGE06.PRG` | 22 | 18886 | 752 | 27 | 27 |
| `RSTAGE07.PRG` | 16 | 10536 | 393 | 12 | 12 |
| `RSTAGE08.PRG` | 19 | 15038 | 674 | 13 | 13 |
| `RSTAGE09.PRG` | 27 | 22145 | 985 | 32 | 32 |
| `RSTAGE10.PRG` | 34 | 21158 | 925 | 29 | 29 |
| `RSTAGE11.PRG` | 14 | 11469 | 438 | 15 | 15 |
| `RSTAGE12.PRG` | 8 | 9204 | 413 | 15 | 15 |
| `RSTAGE13.PRG` | 16 | 13404 | 531 | 14 | 14 |
| `RSTAGE14.PRG` | 5 | 8575 | 342 | 19 | 19 |
| `RSTAGE15.PRG` | 18 | 17168 | 722 | 36 | 36 |
| `RSTAGE16.PRG` | 10 | 12374 | 512 | 15 | 15 |
| `RWARP.PRG` | 5 | 3504 | 134 | 4 | 4 |
| `STAFF.PRG` | 0 | 0 | 0 | 0 | 0 |
| `STAGE_00.PRG` | 15 | 15079 | 518 | 24 | 24 |
| `STAGE_01.PRG` | 12 | 4713 | 149 | 4 | 4 |
| `STAGE_02.PRG` | 14 | 16851 | 654 | 21 | 21 |
| `STAGE_03.PRG` | 8 | 3573 | 117 | 3 | 3 |
| `STAGE_04.PRG` | 32 | 26395 | 1036 | 43 | 43 |
| `STAGE_05.PRG` | 28 | 23454 | 1050 | 33 | 28 |
| `STAGE_06.PRG` | 30 | 22550 | 943 | 26 | 26 |
| `STAGE_07.PRG` | 18 | 11931 | 428 | 17 | 17 |
| `STAGE_08.PRG` | 23 | 22363 | 1016 | 25 | 25 |
| `STAGE_09.PRG` | 9 | 3620 | 98 | 3 | 3 |
| `STAGE_0X.PRG` | 8 | 3297 | 97 | 3 | 3 |
| `STAGE_10.PRG` | 54 | 26967 | 1217 | 39 | 36 |
| `STAGE_11.PRG` | 9 | 4403 | 141 | 3 | 3 |
| `STAGE_12.PRG` | 8 | 10628 | 505 | 22 | 22 |
| `STAGE_13.PRG` | 12 | 4221 | 136 | 3 | 3 |
| `STAGE_14.PRG` | 6 | 10414 | 409 | 57 | 12 |
| `STAGE_15.PRG` | 31 | 20422 | 878 | 28 | 28 |
| `STAGE_16.PRG` | 11 | 10865 | 492 | 18 | 18 |
| `STAGE_EX.PRG` | 10 | 7919 | 372 | 53 | 13 |
| `STAGE_MA.PRG` | 9 | 6267 | 185 | 6 | 6 |
| `SUB.PRG` | 22 | 6721 | 330 | 9 | 9 |
| `TITLE.PRG` | 48 | 8643 | 341 | 11 | 10 |
| `T_BAT.PRG` | 7 | 3268 | 60 | 2 | 2 |
| `T_DEVIL.PRG` | 13 | 6900 | 174 | 6 | 6 |
| `T_DEVIL2.PRG` | 13 | 6898 | 174 | 6 | 6 |
| `T_FAIRY.PRG` | 15 | 8036 | 231 | 13 | 13 |
| `T_FAIRY2.PRG` | 16 | 8074 | 233 | 13 | 13 |
| `T_GHOST.PRG` | 10 | 2412 | 50 | 3 | 3 |
| `T_SWORD.PRG` | 15 | 6464 | 213 | 7 | 7 |
| `WARP.PRG` | 5 | 3384 | 129 | 4 | 4 |

**Totais (79 programas):** funcoes=2709, instrucoes=1119640, jsr_indireto=41388, jmp_indireto=1754, tabelas_de_salto=1448

**Literais de hardware (total / programas que usam):**
- `smpc`: 116 literais em 15 programa(s)
- `minit`: 49 literais em 32 programa(s)
- `sinit`: 2 literais em 2 programa(s)
- `scu_dma`: 6 literais em 1 programa(s)
- `scu_dsp`: 0 literais em 0 programa(s)
- `vdp1_regs`: 22 literais em 1 programa(s)
- `vdp1_vram`: 76 literais em 12 programa(s)
- `vdp2_regs`: 8 literais em 2 programa(s)
- `vdp2_vram`: 112 literais em 15 programa(s)
- `vdp2_cram`: 52 literais em 22 programa(s)
- `scsp`: 36 literais em 2 programa(s)
- `cd_block`: 18 literais em 4 programa(s)
- `sh2_onchip`: 1 literais em 1 programa(s)

Saltos indiretos que NÃO são tabela de salto (candidatos a alvo dinâmico): 306 de 1754

**Cobertura estática [obs]:** instruções desassembladas × 2 B ÷ tamanho = **34 % do total** (0.BIN 47 %; `SUB` 79 %; `ALUCARD` 64 %; vários overlays de fase 5–7 %). Semear como entradas os ponteiros do cabeçalho de `STAGE_01` mudou 12→19 funções e 4713→4749 instruções (+0,8 %): **a baixa cobertura não vem de falta de pontos de entrada.** Ou o resto é dado (tabelas de inimigos, scripts de fase) ou só é alcançável por ponteiros de função em tempo de execução — **não distinguido**.

**Não medido:** código auto-modificável (escritas na própria região de código) e alvo real das 41 388 chamadas `jsr @Rn` (maioria é chamada estática via pool de literais, mas não foi resolvida).

## 3. Leitura dos números
- **Saltos indiretos:** 1 754 `jmp @Rn`, dos quais 1 448 (83 %) são tabelas de salto (`switch`; alvos finitos, resolvíveis) e 306 (17 %) são candidatos a alvo dinâmico.
- **DSP do SCU: 0 literais** [inf, literais não são prova]: não deve ser preciso emular o DSP.
- **SH-2 escravo:** `MINIT` 49 literais em 32 programas, `SINIT` 2 em 2 — ambíguo (`0x01000000` também é constante comum). **A decidir por observação** no núcleo da etapa 2 (log de `SSHON` no SMPC).
- **Periféricos** [obs, subestimam: o código acessa registradores também por base+deslocamento]: SMPC 116 literais em 15 programas, VDP2 VRAM 112 em 15, VDP1 VRAM 76 em 12, VDP2 CRAM 52 em 22, SCSP 36 em 2, CD block 18 em 4, VDP1 regs 22 em 1, VDP2 regs 8 em 2, SCU DMA 6 em 1.

## 4. Recomendação para a etapa 2 [inf, fundamentada acima]
**Interpretador SH-2 primeiro; recompilação guiada por traço depois.** Em vez de “recompilação estática e interpretador de reserva”, inverter a ordem:
1. **Etapa 2:** núcleo SH-2 por interpretador (rápido, com cache de blocos decodificados), memória, SMPC, CD por arquivo (serve `0x0600654C`/diretório `SD` a partir do pacote local), timers/interrupções, entrada. Meta: `0.BIN`+`TITLE.PRG` rodando até a tela de título.
2. **Etapa 8 (ou antes, se o desempenho exigir):** gravar os endereços de código realmente executados (traço) em jogo completo e traduzi-los em AOT para C++ na geração local; o interpretador continua como reserva para o que o traço não cobriu. O critério “sem ISO e sem emulador externo em runtime” permanece atendido.
**Por quê:** 66 % dos bytes de código não têm mapa estático confiável; 306 saltos indiretos não-tabela e 41 388 chamadas indiretas; autoescrita não medida. Uma recompilação puramente estática teria cobertura imprevisível, e o interpretador entrega o jogo rodando cedo (marcos jogáveis) e gera o traço que a recompilação precisa.

## 5. Perguntas abertas (levadas à etapa 2)
SH-2 escravo é usado? Há código auto-modificável? Onde e como o driver decodifica as faixas DVI e qual a taxa? Quem escolhe o destino dos overlays de fase (a rotina `0x060071C8` é um despachante de estados)? Significado dos campos `id/flags/extra` da tabela de estágios e das 25 faixas sem referência estática.
