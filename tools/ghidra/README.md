# Análise no Ghidra (SH-2 cru)

Pré-requisitos: Ghidra 12.2 em `~/opt/ghidra_12.2_PUBLIC` (Java 21+) e o disco extraído em `data-local/disc` (`dxpc-extract <cue> data-local/disc`).

1. Importar e analisar o executável principal (base = *1st read address* do IP.BIN):
   ```
   G=~/opt/ghidra_12.2_PUBLIC/support/analyzeHeadless
   $G "$PWD/data-local/ghidra" dxpc -import "$PWD/data-local/disc/0.BIN" \
      -processor "SuperH:BE:32:SH-2" -cspec default \
      -loader BinaryLoader -loader-baseAddr 0x06004080 -overwrite
   ```
   Resultado esperado: "Analysis succeeded"; ~713 funções.
2. Resumo e desassembly da entrada (ver `EntrySummary.java`):
   ```
   $G "$PWD/data-local/ghidra" dxpc -process 0.BIN -noanalysis \
      -scriptPath "$PWD/tools/ghidra" -postScript EntrySummary.java 0x06004080
   ```
   Esperado: bloco `ram 06004080-060505df` e, na entrada, `mov.l r8,@-r15 / mov.l r14,@-r15 / sts.l pr,@-r15 …`.
3. Interface gráfica: `~/opt/ghidra_12.2_PUBLIC/ghidraRun` e abrir `data-local/ghidra/dxpc`.

Outros `.PRG` (overlays) entram do mesmo modo, com o endereço de carga a descobrir na etapa 1.
