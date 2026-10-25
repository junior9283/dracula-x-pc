# Akumajou Dracula X (Saturn) → port nativo para PC — Design

Data: 2026-10-25 · Status: rascunho para aprovação

## Intenção e porquê
Transformar o jogo de Sega Saturn *Akumajou Dracula X – Gekka no Yasoukyoku* (T-9527G, v1.400, 1998-04-27, JP) em um programa **nativo de PC (Linux e Windows)**, fiel ao original, que abre direto no jogo e se configura inteiramente por menus dentro do próprio jogo. Cada usuário gera o port a partir da **própria imagem de disco**; depois da geração, a ISO não é mais necessária.

## Objetivos
1. **Fidelidade**: comportamento, física, tempos e visual idênticos ao original (referência: execução no Saturn/emulador de referência).
2. **Pipeline de geração local ("build com a sua ISO")**: entrada `.cue/.bin` → saída = executável nativo + pacote de dados convertido para formatos de PC, em pasta local. Execução posterior sem a ISO.
3. **Abrir direto no jogo**; todas as opções dentro do menu do jogo, navegáveis por teclado, mouse e controle.
4. **Entrada**: teclado, mouse, XInput (Xbox), DualShock 4, DualSense, com remapeamento.
5. **Menu de opções (no jogo)**:
   - Salvar/carregar: slots de save em arquivo, import/export, e save states opcionais.
   - Vídeo (padrão de jogos de PC): modo janela/borderless/tela cheia, resolução e escala interna, proporção (4:3 original / 16:9 / esticado), vsync, limite de FPS, filtro (nearest/bilinear/CRT/scanlines), escala inteira, seleção de monitor, brilho/gama.
   - Áudio: volumes master/música/efeitos, dispositivo de saída, taxa de amostragem, latência.
   - Controles: remapeamento, zonas mortas, gatilhos/vibração, ícones por controle.
6. **Sound Test temático ("Galeria da trilha sonora")** no menu: toca cada música do jogo, com um cenário visual **original** cujo tema muda conforme o local do jogo em que a música toca (castelo, biblioteca, capela etc.). Arte e shaders criados do zero; nada copiado do disco.
7. Compilar para **Linux e Windows** (cross-compile mingw-w64 disponível; teste via Wine).

## Não-objetivos
- Distribuir ISO, binários do jogo, imagens, sons ou qualquer dado extraído (nem no repositório, nem em releases).
- Emulador de Saturn genérico para outros jogos (o foco é este título; reutilização é bônus).
- Alterar a jogabilidade original (melhorias ficam como opções desligadas por padrão).

## Anti-objetivos (falha mesmo que "funcione")
- Embutir o disco ou dados do jogo no repositório/release.
- Um "launcher" externo ou janela de configuração separada do jogo.
- Depender da ISO ou de um emulador em tempo de execução após a geração.
- Visual/áudio perceptivelmente diferente do original sem opção explícita.

## Restrições
- Hardware alvo do original: 2× Hitachi SH-2, VDP1 (sprites), VDP2 (fundos), SCU (DSP/DMA), SCSP + 68000 (som), CD-ROM.
- Descoberta da recon: a faixa 2 do disco é áudio de 16 s apenas; **a trilha do jogo está nos dados** (sequências/amostras para o SCSP/68000). O Sound Test depende de entender esse formato e reproduzi-lo (driver de som + SCSP) para gerar áudio de PC.
- Ferramentas disponíveis: cmake, gcc, clang, ninja, python3, Java 25, ffmpeg, mingw-w64, Wine, SDL3 3.4.16, Ghidra 12.2 (`~/opt/ghidra_12.2_PUBLIC`), REA.
- Máquina: 12 núcleos, 15 GB RAM.

## Decisões de "como" (escolhas minhas, o usuário delegou)
- C++20, CMake + Ninja; SDL3 (janela, entrada com gamepad API cobrindo XInput/DS4/DualSense, áudio).
- Renderização via OpenGL 3.3 core (compatível com Linux/Windows/Wine); reimplementação de VDP1/VDP2 em GPU.
- Estratégia: **recompilação estática assistida do código SH-2** (tradução do código do jogo para C++), com camada de hardware reimplementada (VDP1/VDP2/SCU/SCSP/CD→arquivos). Onde a recompilação estática não for viável (código auto-modificável, saltos indiretos), recorrer a um interpretador/JIT SH-2 embutido como fallback, ainda sem exigir ISO em runtime.
- Dados do disco convertidos na geração para um pacote local (imagens, mapas, tabelas, samples, sequências) lido pelo executável.
- Código do repositório: só código original e ferramentas; pasta `build-local/` e `data-local/` ignoradas pelo git.

## Etapas (cada uma com seu plano)
0. Fundação: estrutura do repo, leitor de bin/cue (ISO9660), extração de arquivos do disco, análise inicial no Ghidra/REA (boot, memória, bibliotecas SEGA).
1. Mapeamento do jogo: código principal, overlays, formatos de dados, rotinas de gráficos e de som.
2. Núcleo: CPU SH-2 (recompilador + fallback), memória, SCU, timers, entrada.
3. Vídeo: VDP1 + VDP2 em GPU, janela, opções de vídeo.
4. Áudio: SCSP/68000 (ou driver reimplementado), mixer, opções de áudio.
5. Menu no jogo (opções, controles, saves), tela inicial direta no jogo.
6. Pipeline de geração local ("recompile com a sua ISO") e empacotamento Linux/Windows.
7. Galeria da trilha sonora com temas.
8. Fidelidade, testes de regressão e documentação.

## Deixado ao construtor
Nomes de módulos, formato interno do pacote de dados, layout dos menus, arte dos temas da galeria (original), esquema de testes de fidelidade.

## Riscos
- Tamanho: é um projeto de meses de engenharia; o plano é incremental com marcos jogáveis (boot → tela de título → primeira fase).
- Sons/música dependem de engenharia reversa do driver; pode exigir implementação cycle-approximate do SCSP.
- Legal: o repositório contém apenas código/ferramentas originais; usuário usa mídia que possui.

## Decisões do construtor (após revisão; marcadas como escolha minha)
- **Referência de fidelidade**: Mednafen (Saturn) como referência; comparação por captura de quadros e áudio em pontos fixos de uma gravação de entradas; tolerância por quadro (sincronização por quadro entre os dois SH-2, não ciclo exato), refinando só onde o jogo exigir.
- **Critério de sucesso da execução**: sem ISO e sem emulador externo em runtime. Recompilação estática é a meta; interpretador SH-2 embutido é fallback permitido e vira rede de segurança durante o desenvolvimento.
- **Overlays/código dinâmico**: a geração varre os arquivos do disco, traduz todo código alcançável por descoberta iterativa guiada por execução de referência; o que não for estático cai no fallback.
- **Som**: primeiro emulação do 68000+SCSP (fidelidade); driver de alto nível só se inviável. Tabela música→local vem do mapeamento do jogo (etapa 1) e de observação em execução.
- **Entrada do disco**: aceita apenas T-9527G v1.400 (hash conferido); formatos: cue/bin, ISO crua, CHD pode ficar para depois.
- **Desempenho**: 60 fps estáveis em PC modesto com GPU integrada; OpenGL 3.3.
- **Windows**: verificado via Wine; XInput real, DualSense e vibração ficam como "não verificados" até teste em hardware.
- **Recon já feita**: apenas identificação do disco; Ghidra 12.2 instalado, nenhum projeto anotado ainda.

## Respostas do usuário à revisão
- **Prioridade de corte**: a Galeria da trilha sonora pode ficar para depois se o projeto apertar (não é requisito da primeira versão jogável).
- **Licença / origem do código**: o usuário respondeu "100% nativa de PC" (não nomeou licença). Decisão minha, a confirmar: licença permissiva (MIT) e núcleos escritos do zero, sem copiar código de emuladores GPL (Mednafen, Yabause, Kronos); eles servem só como referência de comportamento observado.

## Revisão após a etapa 1 (decisão do construtor, a confirmar)
- **Estratégia de CPU invertida:** interpretador SH-2 primeiro (etapa 2), recompilação AOT guiada por traço de execução depois; o interpretador permanece como reserva. Motivo: só 34 % dos bytes de código têm mapa estático (docs/analysis/06-mapa-do-jogo.md). O objetivo do usuário (nativo, sem ISO e sem emulador externo em runtime) não muda.
- **Camada de CD:** o jogo carrega por nome de arquivo e diretório; o port serve esses acessos a partir do pacote de dados local.
- **Som:** emular 68000 + SCSP e rodar o driver original (o driver está em `0.BIN`).
- **Música/galeria:** as 49 faixas já são decodificáveis (IMA ADPCM estéreo, 44,1 kHz inferido); a galeria depende da tabela música→local (24/49 por análise estática, resto por log em execução).
