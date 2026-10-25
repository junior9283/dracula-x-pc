# Som, driver e mídia (etapa 1, tarefa 6)

Legenda: **[obs]** observado · **[inf]** inferido. Reproduzir: `ffprobe`/`ffmpeg` (comandos abaixo), `tools/analysis/` e `dxpc-catalog <cue>`.

## 1. Driver de som (68000 + SCSP) [obs]
- Programa do 68000 embutido em `0.BIN` (offset `0x369FC`, endereço `0x0603AA7C`): vetores SP `0x0000A000`, PC `0x00001000`, exceções em `0x1BA6`. Contém apenas tabela de seno (vibrato/LFO) e curva de volume; **sem tabela de ADPCM**.
- Mapa de comunicação observado como literais em `0.BIN`: `0x25A00000` (base da RAM de som, destino do upload), `0x25A00400…0x25A00414` e `0x25A00500` (área de mensagens SH-2↔68K [inf]), `0x25A76000` / `0x25A78000` (buffers no fim da RAM de som), `0x25B00400` (controle do SCSP).
- Rotina de init do som `0x06011CE4`: configura SR, zera estruturas, aponta para o programa do 68000, chama o carregador do driver e abre o diretório `SD` por nome (`"SD"` em `0x06011CE0`).
- **Consequência para a etapa 4:** emular 68000 + SCSP e rodar este driver como está (o jogo conversa com ele pela área de mensagens); não será preciso reimplementar o driver. A decodificação da música DVI (tarefa 3) deve acontecer dentro dessa cadeia (68K ou SH-2), a confirmar na etapa 4.

## 2. Bancos e amostras
- `SD/SD_*.CRT` (55 arquivos, 8,7 MB): começam com tabela de offsets (ex.: `SD_ALK.CRT`: `000E0000 003A0000 00820000 00BE0000 …`, espaçamento ~0x3C–0x44) ou com cabeçalho diferente (`SD_CAT.CRT`: `0001 0000 0006 01E0 …`). Cada nome está em `0.BIN` (a partir de `0x0603D20C`) seguido de um byte de classe (`0x10`, `0x11`, `0x12` …). **[inf]** bancos de timbre/efeito do driver. Formato interno pendente.
- `SD/SDD0–SDD2`, `SD/SDE0–SDE8`, `SD/SDF0` (13 arquivos, 26,6 MB): **[inf]** PCM de **8 bits com sinal** (autocorrelação lag-1: int8 0,28–0,94 contra int16 −0,6…0,8; `SDE8` e `SDD2` têm o mesmo tamanho e as mesmas estatísticas). Taxa desconhecida; `SDF0` (25,7 MB) começa com milhões de zeros.

## 3. Vídeos (`*.AVI`, 4 arquivos, 28,8 MB) [obs]
| Arquivo | Vídeo (ffmpeg) | Cabeçalho AVI | Quadros/s | Áudio |
|---|---|---|---|---|
| `OPENING.AVI` | `truemotion1` 140×144 | 280×144 | 12 | `adpcm_ima_dk4` 44,1 kHz estéreo |
| `ENDING.AVI` | `truemotion1` 140×144 | 280×144 | 15 | `adpcm_ima_dk4` 44,1 kHz estéreo |
| `BREAK.AVI` | `truemotion1` 140×144 | 280×144 | 15 | `adpcm_ima_dk4` 44,1 kHz estéreo |
| `KONAMI.AVI` | `truemotion1` 160×196 | 320×199 | ~15 | `pcm_s16le` 22,05 kHz mono |

**Divergência resolvida [obs]:** o ffmpeg decodifica metade da largura (pixels de proporção 2:1); escalar até as dimensões do cabeçalho AVI produz um quadro natural (verificado em `OPENING.AVI` aos 20 s a 280×144). Receita de conversão para o pacote local (sem codec proprietário no PC):
```
ffmpeg -i OPENING.AVI -vf "scale=280:144:flags=neighbor" -c:v libx264 -crf 18 -c:a aac OPENING.mp4
```
Use largura×altura do `avih` de cada arquivo (`KONAMI`: `scale=320:199`).

## 4. Bitmaps de bônus (`OMAKE/`) [obs]
`OMAKE/24/*.BMP` (24 bpp, 576×768 e `DX01` maior) e `OMAKE/8/*.BMP` (8 bpp): 30 BMPs, 27,9 MB, Windows BMP padrão — legíveis sem conversão. `OMAKE/KAIHATSU.DOC`: texto Shift-JIS (comentários da equipe), 11 KB.

## 5. Pendências
Formato dos `.CRT`; taxa e uso dos PCM de 8 bits; como o driver decodifica/toca os `SD##.PCM` (etapa 4).
