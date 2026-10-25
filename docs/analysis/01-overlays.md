# Carregador de arquivos e overlays (etapa 1, tarefa 2)

Legenda: **[obs]** observado no código/dados · **[inf]** inferido · **[pad]** convenção. Reproduzir: `tools/ghidra/FileLoaderTrace.java`, `tools/analysis/overlay_bases.py`, `dxpc-overlays <cue>`.

## 1. Rotina genérica de carga [obs]
`0x0600654C(r4 = ponteiro para nome, r5 = endereço de destino)` lê um arquivo do CD para o destino. Exemplo em `0x06005470`: `r5 = 0x060A5000`, `r4 = "LOGO.PRG"` (`0x06005458`). Os nomes ficam em strings `NOME.EXT\0` entre `0x06005458` e `0x06020BF0`, intercaladas com o código das rotinas de cena (328 nomes; 264 com referência direta; 5 `.PRG`, 4 `.MAP`, 3 `.CHR` com destino literal ao lado do nome).

Destinos literais lidos do código [obs]:
| Arquivo | Destino | Região |
|---|---|---|
| `LOGO.PRG`, `TITLE.PRG`, `STAFF.PRG` | `0x060A5000` | Work RAM-H |
| `MOVIE.PRG` | `0x06066000` | Work RAM-H |
| `ENDING.PRG` | `0x060DC000` | Work RAM-H |
| `*.MAP` (títulos/carregamento) | `0x00252000`, `0x00254000`, `0x00256000` | Work RAM-L |
| `*.CHR` | `0x25C13980`, `0x25C2A980` | VDP1 VRAM (espelho sem cache de `0x05C00000`) |
| `ASCII.FON` | `0x00252000` | Work RAM-L |

## 2. Tabela de estágios [obs]
`0x06047818 … 0x060481C0`: 103 registros de 24 bytes `{ptr .PRG, ptr .CHR, ptr .MAP, id, flags, extra}`; os ponteiros são para os nomes. Cobre `STAGE_nn`, `RSTAGEnn` (castelo invertido), `BOSS_nn`, `BOSS_Rnn`, `WARP/RWARP`, `LIBRARY`, `STAGE_MA/EX/0X`. Muitos registros repetem `STAGE_01` (id `0xB06`, flags `0xFF000000`) — [inf] espaços reservados. `id` (`0xB03`–`0xB37`) tem faixa compatível com um índice de banco de som/música (55 arquivos `SD_*.CRT`); a confirmar na tarefa 4. As últimas 3 entradas (`0x06048178…`) são tabelas de nomes de `.MAP` alternativos (A/B), não registros.

## 3. Endereço de carga dos overlays [obs, validado]
Para os overlays sem destino literal, a base é estimada por votação (ponteiro − deslocamento de prólogo `sts.l pr,@-r15`) em `tools/analysis/overlay_bases.py`. **Validação:** nos 5 casos com destino literal conhecido o método acerta exatamente (`0x060A5000`×3, `0x06066000`, `0x060DC000`), com votos 3–131 contra 1–12 do segundo colocado.

| Grupo | Base | Arquivos |
|---|---|---|
| Fases e chefes | `0x060DC000` | `STAGE_*`, `RSTAGE*`, `BOSS_*`, `BOSS_R*`, `WARP`, `RWARP`, `LIBRARY`, `ENDING` (60) |
| Cenas e personagens | `0x060A5000` | `LOGO`, `OPEN`, `STAFF`, `TITLE`, `ALUCARD`, `MARIA`, `RICHTER`, `RICHTER2` (8) |
| Jogo / vídeo | `0x06066000` | `GAME`, `MOVIE` (alternam) |
| Familiares | `0x060CF000` | `T_BAT`, `T_DEVIL`, `T_DEVIL2`, `T_FAIRY`, `T_FAIRY2`, `T_GHOST`, `T_SWORD` (7) |
| Submenu | `0x060D6000` | `SUB` |
| Dados | — | `WEAPON0.PRG`, `WEAPON1.PRG` (786 432 B cada; votos empatados → não são código) |

Sobreposições com base diferente (`dxpc-overlays`) [obs]: apenas `MARIA/RICHTER/RICHTER2` × `T_*`. [inf] Esses personagens nunca carregam os familiares do Alucard (o `ALUCARD.PRG` termina em `0x060CE480`, antes de `0x060CF000`). **A confirmar** na tarefa 7 pela tabela de chamadas.

## 4. Formato do cabeçalho de `.PRG` [obs]
O arquivo começa com uma tabela de ponteiros de 32 bits (big-endian) para dentro do próprio overlay, terminada por palavra não-ponteiro (ex.: `STAGE_00.PRG`: 10 ponteiros `0x060EB1BC…`, depois `0`; `TITLE.PRG`: 3). `parse_overlay` devolve essa tabela; o significado de cada entrada (pontos de entrada/ganchos por fase) é pendência da tarefa 7.

## 5. Pendências
- Onde `GAME.PRG` é carregado (valor literal) e quem escolhe o destino dos overlays de fase (a rotina `0x060071C8` é um despachante de estado, não o carregador).
- Significado de `id`/`flags`/`extra` da tabela de estágios.
