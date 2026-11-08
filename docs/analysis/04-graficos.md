# Gráficos: `.CHR`, `.MAP`, `.PTN` (etapa 1, tarefa 5)

Legenda: **[obs]** observado · **[inf]** inferido. Reproduzir: `python3 -I tools/analysis/chr_sheet.py data-local/disc/ITEM.CHR folha.png`.

## Escopo (decisão)
O projeto **executa o código original do jogo** (interpretado pelo núcleo; a recompilação para código de PC é a meta de longo prazo e ainda não foi feita); os descompressores e as rotinas de VDP1/VDP2 do jogo rodam como estão. O formato dos gráficos só é necessário para ferramentas fora do fluxo do jogo (verificação, galeria). Portanto esta tarefa documenta o que foi confirmado e **não** implementa decodificadores em C++.

## Confirmado [obs]
- **`ITEM.CHR` (76 800 B) = pixel cru de 4 bpp**: linhas de 8 bytes (16 px), nibble **alto** = pixel à esquerda, **600 sprites 16×16 empilhados** (128 B cada). Validação visual: a folha gerada mostra ícones nítidos (armas, poções, comidas, armaduras, telas “DUMMY” do desenvolvedor). A paleta usada na verificação é arbitrária (o jogo usa a Color RAM).
- Os `.CHR` do título (`TITLE.CHR`, `OPENNING.CHR`) são carregados direto na **VDP1 VRAM** (`0x25C13980`, `0x25C2A980`, espelho sem cache de `0x05C00000`) pela rotina `0x0600654C` (ver `01-overlays.md`) — portanto já estão no formato que a VDP1 consome.
- **18 arquivos `.CHR`** (`S011`, `S030`, …, `SDRACULA`, `SWATA`, `SBO7`) usam só **2 valores de nibble** (0 e 1); cada um tem um `S###.PTN` de mesmo nome. Largura de linha ainda desconhecida (a hipótese de 4 bpp linear com larguras 32/64/128/256 não produziu formas reconhecíveis).
- **81 `.CHR` usam os 16 valores**. 86 dos 99 `.CHR` começam com 3 zeros; um (`ALUCARD.CHR`) começa com `FD 00 DE …`.
- **Assinatura `FD 00 DE`** aparece em `ALUCARD.CHR` e em **39 dos 77 `.MAP`** (ex.: `STAGE_00.MAP`: `fd 00 de 7e 44 45 …`). **[inf]** é um esquema de compressão com marcador; a descompressão ocorre no código SH-2 do jogo.
- `STAGE_00.CHR` começa com uma tabela de pares `u32` (`0x0000000D 0x0`, `0x00E0000C 0x0`, …) antes do pixel — layout por fase ainda sem decodificar.
- `.PTN`: 18 arquivos de até 5 632 B, sequência de `u16` crescentes (`0001 0002 0003 …`, `S011.PTN`): **[inf]** tabelas de índices de quadros/células de animação; significado exato pendente.

## O que NÃO foi determinado
Compressão `FD 00 DE`; layout dos `.CHR` de fase/chefe/personagem; formato dos `.MAP` (blocos de mapa de fundo da VDP2); paletas. **Não bloqueiam o núcleo (etapa 2)**, que usa o código original; só bloqueiam ferramentas de extração/visualização.
