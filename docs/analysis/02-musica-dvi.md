# Música em fluxo `SD/SD01.PCM … SD31.PCM` (formato DVI) — etapa 1, tarefa 3

Legenda: **[obs]** observado · **[inf]** inferido · **[pad]** convenção. Reproduzir: `dxpc-music-export <cue> data-local/music` e `python3 -I tools/analysis/dvi_probe.py data-local/disc/SD SD03.PCM`.

## Formato [obs, validado]
49 arquivos com magic `DVI.` (nomes `SD01`–`SD31` em hexadecimal). Todos satisfazem `header_size (0x800) + body_size == tamanho do arquivo`.

| Offset | Campo (big-endian) |
|---|---|
| `+0x00` | `"DVI."` |
| `+0x04` | tamanho do cabeçalho = `0x800` |
| `+0x08` | tamanho do corpo em bytes (= quadros estéreo: 1 byte por quadro) |
| `+0x0C` | **[inf]** início do trecho em laço, em quadros (ex.: `SD01` = 21,1 s; `SD04` = 112,1 s a 44,1 kHz) |
| `+0x10` | `int16` predictor L; `+0x12` = `0x7FFF` em todas as faixas (ignorado) |
| `+0x14` | índice de passo L (0..88) |
| `+0x20` | `int16` predictor R; `+0x22` = `0x0000` em todas (ignorado) |
| `+0x24` | índice de passo R (0..88) |

Corpo: **IMA/DVI ADPCM estéreo** (tabela de passos padrão de 89 entradas, ajuste `-1,-1,-1,-1,2,4,6,8`). Intercalação: grupos de 8 bytes = 4 bytes do canal L + 4 bytes do canal R; dentro de cada byte o **nibble alto vem primeiro**. Estado inicial de cada canal vem do cabeçalho.

## Como o formato foi determinado
- Evidência estatística: histograma de nibbles típico de IMA (códigos pequenos dominam, simétrico em sinal); índices do cabeçalho sempre em 0..88.
- Critério decisivo: **fração da energia abaixo de 20 Hz** (um decodificador errado vira passeio aleatório com 86–98% nessa faixa; música correta tem ~0%). `tools/analysis/dvi_probe.py` testou 4 + 9×2 intercalações × {IMA, Yamaha ADPCM-B}: só “IMA, grupos de 4 bytes, nibble alto primeiro” chegou a 0,00% (o segundo melhor: 14%).
- **Lição:** autocorrelação de lag 1, energia de alta frequência e correlação L/R **não** discriminam (passeios aleatórios também as satisfazem). O erro inicial (nibble baixo primeiro) passou por elas e só foi pego pela energia subsônica.
- Resultado nas 49 faixas decodificadas: energia <20 Hz mediana 0,001% (máx. 1,5%, `SD16`), RMS mediano 4138, clipping ≤ 0,02%, |DC| ≤ 4440.

## Pendências / incertezas
- **Taxa de amostragem: 44 100 Hz é inferência** (`audio::kDviSampleRate`); o cabeçalho não a informa e não achei a rotina de decodificação no código do jogo. Evidências a favor: 99,99% da energia abaixo de 4–18 kHz; durações de 2 a 5 min coerentes com trilhas de jogo. Se a taxa real for outra, as faixas soarão com a velocidade/tom trocados — confirmar na tarefa 6 pelo código do driver.
- Significado exato de `+0x0C` (laço) a confirmar ouvindo/observando o ponto de laço.
- Onde o decodificador roda: o programa do 68000 do SCSP **não** contém tabela IMA nem de ADPCM (apenas tabela de seno e curva de volume); nenhuma tabela IMA existe em arquivo ≤2 MB do disco. Logo a decodificação deve ser calculada (ou ocorrer fora dos arquivos varridos) — a investigar na tarefa 6.

## Driver de som encontrado [obs] (insumo da tarefa 6)
- Programa do 68000 embutido em `0.BIN`, offset `0x369FC` = endereço `0x0603AA7C` (vetores: SP `0xA000`, PC `0x1000`); a rotina `0x06011CE4` (init do som) o referencia, usa a RAM de som `0x25A76000/0x25A78000` e abre o diretório `SD` por nome (`"SD"` em `0x06011CE0`) — as faixas são selecionadas por índice, daí não haver nomes `SD##.PCM` no código.
- `SD_*.CRT` (55 arquivos) têm nomes embutidos em `0.BIN` a partir de `0x0603D20C`, cada um com um byte de classe (`0x10`, `0x11`, `0x12`…).
