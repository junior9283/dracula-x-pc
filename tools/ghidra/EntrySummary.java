// Resumo do programa aberto: nº de funções, blocos de memória e desassembly a partir do ponto de entrada.
// Uso: analyzeHeadless <projeto> dxpc -process <arquivo> -noanalysis -scriptPath tools/ghidra -postScript EntrySummary.java 0x06004080
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class EntrySummary extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Address entry = toAddr(args.length > 0 ? args[0] : "0x06004000");

        println("funções: " + currentProgram.getFunctionManager().getFunctionCount());
        for (MemoryBlock block : currentProgram.getMemory().getBlocks())
            println("bloco " + block.getName() + " " + block.getStart() + "-" + block.getEnd());

        if (getInstructionAt(entry) == null) {  // BinaryLoader não define ponto de entrada
            disassemble(entry);
            createFunction(entry, null);
        }
        println("funções (após entrada): " + currentProgram.getFunctionManager().getFunctionCount());
        Instruction insn = getInstructionAt(entry);
        for (int i = 0; i < 40 && insn != null; i++, insn = insn.getNext())
            println(insn.getAddress() + "  " + insn);
    }
}
