// Desassembla um intervalo inteiro, palavra a palavra: args = <endereço-inicial> <endereço-final>. Útil para dumps de RAM sem
// pontos de entrada (código descompactado em tempo de execução). Não grava no projeto com -readOnly.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class DisasmRange extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Address start = toAddr(args[0]), end = toAddr(args[1]);
        for (Address a = start; a.compareTo(end) < 0; a = a.add(2))
            if (getInstructionAt(a) == null) disassemble(a);
        for (Address a = start; a.compareTo(end) < 0; a = a.add(2)) {
            Instruction insn = getInstructionAt(a);
            if (insn != null) println(a + "  " + insn);
        }
    }
}
