# Boot, arquivos do disco e mapa de memória (etapa 0)

Disco: T-9527G V1.400, 1998-04-27, área `J`. Tudo abaixo vem do disco do autor, extraído localmente com `dxpc-extract` (nada disto é versionado).
Legenda: **[obs]** observado nos dados · **[inf]** inferido · **[pad]** convenção padrão do Saturn.

## 1. Cabeçalho / IP.BIN (setores 0–15)
| Campo (offset) | Valor | |
|---|---|---|
| IP size (0xE0) | `0x1800` | [obs] 3 setores de código inicial |
| Stack-M / Stack-S (0xE8/0xEC) | `0` | [obs] pilhas não definidas no cabeçalho |
| 1st read address (0xF0) | `0x06004080` | [obs] |
| 1st read size (0xF4) | `0` | [obs] (usa o tamanho do arquivo) |
| Código do IP (0x100…) | SH-2 (`4f22` = `sts.l pr,@-r15`) | [obs] |
| IP carregado em | `0x06002000` | [pad] referências internas `0x0600xxxx` são coerentes |

## 2. Executável principal: `0.BIN`
- 312 672 bytes, carregado em **`0x06004080`** (= 1st read address) → ocupa `0x06004080–0x060505DF` na Work RAM-H.
- **Prova do endereço-base** [obs]: dos ponteiros internos do arquivo, 618 alvos distintos caem em prólogos de função SH-2 com base `0x06004080`, contra 53 com `0x06004000` (e 68/60/40 com 0x4100/0x3F80/0x4040). Com base errada o Ghidra achou 363 funções; com a certa, 713.
- Entrada (`0x06004080`): `main`. Salva r8/r14/PR, chama inicialização, ajusta registradores de hardware e entra em laço infinito `jsr @r8 / bra` (rotina principal por quadro) [obs].

## 3. Arquivos do disco (436 extraídos; 1 ignorado)
| Padrão | Qtde aprox. | Papel [inf] |
|---|---|---|
| `0.BIN` | 1 | executável principal (acima) |
| `*.PRG` | muitos (`STAGE_xx`, `RSTAGEnn`, `BOSS_xx`, `GAME`, `TITLE`, `OPEN`, `LOGO`, `MOVIE`, `STAFF`…) | overlays de código SH-2 carregados por fase/tela |
| `*.CHR` / `*.MAP` | muitos | gráficos (tiles/sprites) e mapas de fase |
| `S###.CHR` / `.PTN` | ~20 pares | sprites de inimigos/objetos e padrões de animação |
| `SD/SD##.PCM` | dezenas | amostras de áudio (SCSP) |
| `*.AVI` | 4 (`OPENING`, `ENDING`, `BREAK`, `KONAMI`) | vídeos |
| `KANJI.FON`, `ASCII.FON` | 2 | fontes |
| `EVENT*.00x`, `OMAKE/`, `*.TXT` | poucos | eventos e textos |
| `CD3;1` | 1 (2 191 360 B, LBA 204 963) | **ignorado**: arquivo fantasma sobre a faixa de áudio (fora da faixa de dados) |

Consequências para o design: a música não está em áudio de CD (a faixa 2 tem ~16 s); o som vem de `SD/*.PCM` + dados de sequência (a localizar na etapa 1). O código é carregado por **overlays** `.PRG` — a recompilação precisa traduzir cada `.PRG`.

## 4. Mapa de memória do Saturn usado [pad]
| Região | Endereço |
|---|---|
| BIOS ROM | `0x00000000` |
| SMPC | `0x00100000` |
| Backup RAM | `0x00180000` |
| Work RAM-L (1 MB) | `0x00200000` |
| A-bus (cartucho/CD block) | `0x02000000…` |
| SCSP: RAM de som / registradores | `0x05A00000` / `0x05B00000` |
| VDP1 VRAM / framebuffer / regs | `0x05C00000` / `0x05C80000` / `0x05D00000` |
| VDP2 VRAM / Color RAM / regs | `0x05E00000` / `0x05F00000` / `0x05F80000` |
| SCU regs | `0x05FE0000` |
| Work RAM-H (1 MB) | `0x06000000` |

## 5. Ferramentas e decisões
- **REA não abre binário SH-2 cru** (`unsupported binary format`): a análise do código usa o Ghidra headless direto (`SuperH:BE:32:SH-2`, `BinaryLoader`, base informada). Procedimento em `tools/ghidra/README.md`.
- Ghidra 12.2 em `~/opt/ghidra_12.2_PUBLIC`; projeto em `data-local/ghidra/` (ignorado pelo git).

## 6. Pendências para a etapa 1
Mapear o código do IP.BIN (carga de `0.BIN`), tabela de overlays (qual `.PRG` vai para qual endereço), formato de `.CHR/.MAP/.PTN`, driver de som e onde ficam as sequências de música, formato dos `.AVI`.
