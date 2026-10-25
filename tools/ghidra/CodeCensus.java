// Censo do programa aberto: funções, instruções, chamadas/saltos indiretos (jsr/jmp @Rn), tabelas de salto (mova perto de
// jmp), retornos (rts/rte) e literais de hardware carregados por `mov.l @(disp,pc),Rn`, agrupados por periférico.
// Uma linha final "CENSO <programa> chave=valor ...". Uso: analyzeHeadless <projeto> dxpc -import <arq> ... -postScript CodeCensus.java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class CodeCensus extends GhidraScript {
    private static final Pattern PC_LOAD = Pattern.compile("mov\\.l @\\((0x[0-9a-f]+),pc\\),r\\d+");
    private static final long CACHE_THROUGH = 0x20000000L;
    private static final long ON_CHIP = 0xFFFFFE00L;

    // Faixas sobre o endereço normalizado (sem o bit de cache-through), em ordem de NAMES.
    private static final long[][] RANGES = {
        {0x00100000L, 0x00100080L}, {0x01000000L, 0x01000004L}, {0x01800000L, 0x01800004L},
        {0x05FE0000L, 0x05FE0060L}, {0x05FE0080L, 0x05FE0090L}, {0x05D00000L, 0x05D00020L}, {0x05C00000L, 0x05CC0000L},
        {0x05F80000L, 0x05F80120L}, {0x05E00000L, 0x05E80000L}, {0x05F00000L, 0x05F01000L}, {0x05A00000L, 0x05C00000L},
        {0x05800000L, 0x05900000L}, {ON_CHIP, 0x100000000L}};
    private static final String[] NAMES = {"smpc", "minit", "sinit", "scu_dma", "scu_dsp", "vdp1_regs", "vdp1_vram", "vdp2_regs",
        "vdp2_vram", "vdp2_cram", "scsp", "cd_block", "sh2_onchip"};

    private static long normalize(long address) {
        if (address >= ON_CHIP) return address;
        return (address & CACHE_THROUGH) != 0 ? address & ~CACHE_THROUGH : address;
    }

    @Override
    protected void run() throws Exception {
        long insns = 0, indirectCalls = 0, indirectJumps = 0, jumpTables = 0, returns = 0, lastMova = Long.MIN_VALUE;
        long[] hardware = new long[NAMES.length];
        for (InstructionIterator it = currentProgram.getListing().getInstructions(true); it.hasNext(); ) {
            String text = it.next().toString();
            insns++;
            if (text.startsWith("jsr @")) indirectCalls++;
            if (text.startsWith("mova")) lastMova = insns;
            if (text.startsWith("jmp @")) {
                indirectJumps++;
                if (insns - lastMova <= 4) jumpTables++;
            }
            if (text.startsWith("rts") || text.startsWith("rte")) returns++;

            Matcher literal = PC_LOAD.matcher(text);
            if (!literal.find()) continue;
            long address = normalize(getInt(toAddr(literal.group(1))) & 0xffffffffL);
            for (int k = 0; k < RANGES.length; k++)
                if (address >= RANGES[k][0] && address < RANGES[k][1]) hardware[k]++;
        }
        StringBuilder line = new StringBuilder("CENSO " + currentProgram.getName()
            + " funcoes=" + currentProgram.getFunctionManager().getFunctionCount() + " instrucoes=" + insns
            + " jsr_indireto=" + indirectCalls + " jmp_indireto=" + indirectJumps + " tabelas_de_salto=" + jumpTables
            + " retornos=" + returns + " bytes=" + currentProgram.getMemory().getSize());
        for (int k = 0; k < NAMES.length; k++) line.append(' ').append(NAMES[k]).append('=').append(hardware[k]);
        println(line.toString());
    }
}
