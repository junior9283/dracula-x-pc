// Para cada nome de arquivo (NOME.EXT\0) embutido no programa, lista as referências a ele (de onde e em qual função).
// Uso: analyzeHeadless <projeto> dxpc -process 0.BIN -noanalysis -readOnly -scriptPath tools/ghidra -postScript FileLoaderTrace.java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class FileLoaderTrace extends GhidraScript {
    private static final Pattern NAME = Pattern.compile("[A-Z0-9_]{1,12}\\.(CHR|MAP|PRG|PCM|PTN|AVI|FON|CRT)\0");

    private static final Pattern PC_LOAD = Pattern.compile("mov\\.l @\\((0x[0-9a-f]+),pc\\),(r\\d+)");

    // Valor do literal carregado por `mov.l @(x,pc),<reg>` entre as instruções vizinhas de `at` (anteriores ou seguintes).
    private String literalFor(Address at, String reg) throws Exception {
        Instruction i = getInstructionAt(at);
        for (int k = 0; k < 4 && i != null; k++) {
            i = i.getPrevious();
            if (i == null) break;
            Matcher m = PC_LOAD.matcher(i.toString());
            if (m.find() && m.group(2).equals(reg)) return "0x" + Long.toHexString(getInt(toAddr(m.group(1))) & 0xffffffffL);
        }
        return "?";
    }

    @Override
    protected void run() throws Exception {
        MemoryBlock block = currentProgram.getMemory().getBlocks()[0];
        byte[] bytes = new byte[(int) block.getSize()];
        block.getBytes(block.getStart(), bytes);
        Matcher m = NAME.matcher(new String(bytes, java.nio.charset.StandardCharsets.ISO_8859_1));
        int total = 0, referenced = 0;
        while (m.find()) {
            Address addr = block.getStart().add(m.start());
            total++;
            StringBuilder refs = new StringBuilder();
            for (Reference ref : getReferencesTo(addr)) {
                Function f = getFunctionContaining(ref.getFromAddress());
                refs.append(ref.getFromAddress()).append("[r5=").append(literalFor(ref.getFromAddress(), "r5")).append("] ");
            }
            if (refs.length() > 0) referenced++;
            println(addr + " " + m.group().trim() + " <- " + refs);
        }
        println("nomes: " + total + ", com referência direta: " + referenced);
    }
}
