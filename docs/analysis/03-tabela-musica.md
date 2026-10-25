# Tabela música → local (etapa 1, tarefa 4)

Legenda: **[obs]** observado · **[inf]** inferido. Reproduzir: `python3 -I tools/analysis/stage_music_table.py data-local/disc`.

## Método e confiança
A tabela de estágios do `0.BIN` (`0x06047818`, 24 bytes por registro; ver `01-overlays.md` §2) traz um campo `flags` com prefixo `0xE0` nos registros de fase. **[inf]** O byte baixo desse campo é o número da faixa `SD<hex>.PCM`. Evidências: nos 35 registros com prefixo `0xE0`, o byte baixo é sempre uma faixa que existe no disco (35/35); aparecem pares coerentes (`RSTAGE12` repete `STAGE_12`; `STAGE_01` e `STAGE_0X` coincidem); a ocupação cobre 24 das 49 faixas.

## Tabela (gerada)
| Overlay | id | faixa |
|---|---|---|
| `STAGE_06` | `0xB0E` | `SD01.PCM` |
| `STAGE_10` | `0xB18` | `SD02.PCM` |
| `RSTAGE12` | `0xB13` | `SD03.PCM` |
| `STAGE_12` | `0xB12` | `SD03.PCM` |
| `STAGE_02` | `0xB08` | `SD04.PCM` |
| `STAGE_13` | `0xB14` | `SD05.PCM` |
| `STAGE_05` | `0xB0C` | `SD06.PCM` |
| `STAGE_09` | `0xB10` | `SD07.PCM` |
| `RSTAGE07` | `0xB1D` | `SD08.PCM` |
| `STAGE_07` | `0xB1C` | `SD08.PCM` |
| `STAGE_03` | `0xB06` | `SD09.PCM` |
| `STAGE_01` | `0xB04` | `SD0A.PCM` |
| `STAGE_0X` | `0xB03` | `SD0A.PCM` |
| `STAGE_08` | `0xB1A` | `SD0B.PCM` |
| `STAGE_04` | `0xB0A` | `SD0C.PCM` |
| `STAGE_11` | `0xB16` | `SD0D.PCM` |
| `RSTAGE14` | `0xB2D` | `SD0E.PCM` |
| `STAGE_14` | `0xB32` | `SD0E.PCM` |
| `RSTAGE05` | `0xB0D` | `SD10.PCM` |
| `RSTAGE06` | `0xB0F` | `SD10.PCM` |
| `RSTAGE09` | `0xB11` | `SD10.PCM` |
| `RSTAGE11` | `0xB17` | `SD11.PCM` |
| `RSTAGE13` | `0xB15` | `SD12.PCM` |
| `RSTAGE01` | `0xB05` | `SD17.PCM` |
| `RSTAGE02` | `0xB09` | `SD17.PCM` |
| `RSTAGE03` | `0xB07` | `SD17.PCM` |
| `RSTAGE04` | `0xB0B` | `SD17.PCM` |
| `RSTAGE08` | `0xB1B` | `SD17.PCM` |
| `RSTAGE10` | `0xB19` | `SD17.PCM` |
| `STAGE_00` | `0xB30` | `SD19.PCM` |
| `STAGE_15` | `0xB35` | `SD25.PCM` |
| `STAGE_16` | `0xB36` | `SD26.PCM` |
| `RSTAGE15` | `0xB34` | `SD29.PCM` |
| `BOSS_15` | `0xB06` | `SD2B.PCM` |
| `RSTAGE16` | `0xB37` | `SD2C.PCM` |

Faixas distintas: 24 de 49. Sem referência na tabela: `SD0F`, `SD13`, `SD14`, `SD15`, `SD16`, `SD18`, `SD1A`, `SD1B`, `SD1C`, `SD1D`, `SD1E`, `SD1F`, `SD20`, `SD21`, `SD22`, `SD23`, `SD24`, `SD27`, `SD28`, `SD2A`, `SD2D`, `SD2E`, `SD2F`, `SD30`, `SD31`

## O que falta
- **25 faixas não passam por esta tabela** (chefes, título, abertura, créditos, game over, biblioteca etc.). A seleção delas está no código dos overlays; localizar a rotina de “tocar música” por análise estática se mostrou caro. **Decisão:** completar a tabela por **registro em tempo de execução** (log das requisições de faixa por cena) assim que o núcleo da etapa 2 rodar o jogo; é a fonte de verdade e dispensa a engenharia reversa de cada chamada.
- Nomes de exibição dos locais ficam **[inf]** até haver texto do próprio jogo associado (as telas de título/nome de área); os temas da galeria (etapa 7) usarão os nomes dos overlays como chave e arte original.
