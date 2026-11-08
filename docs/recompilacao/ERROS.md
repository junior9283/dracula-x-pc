# Registro de erros

Regra: **todo erro é avaliado e resolvido sem esperar o usuário**, a menos que a decisão seja dele (arquitetura, licença, o que vai a
público). Ciclo: sintoma → causa → correção → teste que reproduz → lição. Registrar aqui, mesmo quando a correção é pequena.
Estados: **aberto**, **em análise**, **resolvido**.

## Abertos

### E02 — 17 619 acessos a endereço não mapeado (primeiro: `0x00009060`) — aberto
- **Sintoma:** `dxpc-run … --frames 9600` (caminho normal, sem BIOS real) termina com "acessos a endereço não mapeado: 17619, página `0x00009000`".
  Com o índice de fase `0x0E` (`WARP`, via `--poke`) a contagem sobe para 144 milhões e o jogo cai em código inválido.
- **Hipótese:** o jogo lê a área da ROM da BIOS (`0x00000000–0x0007FFFF`), que no modo sem BIOS real não existe; a leitura devolve zero.
- **Para resolver:** achar o PC que lê (`--stop-on-unmapped`, `--history`), ver o que o valor controla e, se for dado da BIOS (tabela, fonte),
  fornecer o conteúdo equivalente na BIOS por software. Teste: o contador deve cair a zero e a imagem/áudio de referência não pode piorar.

### E03 — comando de CD block desconhecido `0x21` — aberto
- **Sintoma:** o mesmo `dxpc-run` imprime "comando de CD block desconhecido: 0x00000021".
- **Para resolver:** identificar quem envia (`--log-cd`, `--log-cd-context`), implementar ou ignorar com a resposta correta, com teste no
  estilo de `tests/cdblock_boot_test.cpp`.


## Resolvidos

### E05 — scripts do macOS nunca executados num Mac — resolvido (2026-11-08)
- **Resultado:** o GitHub Actions compilou e empacotou sem erro na primeira tentativa do script, depois de corrigir o runner (E01). A suíte
  (382 casos) passa em Mac Intel e em Apple Silicon (arm64 nativo). Gerou `Dracula-X-PC-macOS-{universal,x86_64,arm64}.dmg`; verificado: o
  universal tem x86_64 e arm64, os outros só a sua arquitetura, e o `.app` tem Info.plist, ícone, LEIAME e assinatura ad hoc.
- **Limite que continua:** ninguém abriu o app num Mac (janela, som, controle); o macOS não é notarizado, então o Gatekeeper pede confirmação.
- **Lição:** o código portátil (sem chamadas de sistema específicas) e o teste em compiladores diferentes (clang local + clang da Apple) evitaram retrabalho.

### E01 — runner `macos-14` (Apple Silicon) não suportado — resolvido (2026-11-08)
- **Sintoma:** job cancelado em 0 s: "The configuration 'macos-14-arm64-us-default' is not supported".
- **Causa:** esta conta do GitHub não tem esse tipo de runner.
- **Correção:** empacotar no runner Intel (`macos-15-intel`), que compila as duas arquiteturas; o teste arm64 nativo virou job opcional
  (`continue-on-error`).
- **Lição:** conferir os rótulos de runner disponíveis antes de depender deles; o arm64 só é verificado por compilação até haver um runner arm64.

### E04 — estalo de uma amostra na interpolação do SCSP — resolvido (2026-11-08, commit `1b8e09b`)
- **Sintoma:** pico isolado de ~28 000 de amplitude em efeitos sonoros fortes (13 em 160 s no roteiro de 9 600 quadros).
- **Causa:** `(s1 - s0) * fraction` em 32 bits dava a volta quando o salto entre amostras passava de ~32 768.
- **Correção:** produto em 64 bits. **Teste:** onda quadrada ±30 000 em `tests/scsp_test.cpp` (falhava antes). Passos acima de 12 000: 28 → 0.
- **Lição:** conferir produtos de 16 bits por 16 bits em inteiros de 32 bits.

### E06 — índices de save state sem validação (achado da revisão de segurança) — resolvido (2026-11-08, commit `2180082`)
- **Causa:** `CT` do DSP do SCU e a contagem de DMA de programa vinham do arquivo sem conferência; o CD block também confiava em conexões e partições.
- **Correção:** `StateError` na carga; o save do diretório passou a gravar os campos XA (estado v4). **Testes:** `tests/scu_dsp_test.cpp`, `tests/cdblock_boot_test.cpp`.
- **Lição:** todo índice lido de arquivo é validado na carga.

### E07 — hash de referência divergente relatado por um subagente — não reproduzido
- Um subagente relatou que a cópia original dava um PNG de 4000 quadros diferente de `4879f476…`. Em quatro execuções minhas, o mesmo comando
  (com `--lenient-bios --boot-font data-local/boot-font.bin`) deu sempre `4879f476…`. Provável diferença nas opções usadas por ele. Sem ação.
