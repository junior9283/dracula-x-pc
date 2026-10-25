// Desassembla um intervalo: args = <endereço-inicial> <nº de instruções>. Não grava no projeto com -readOnly.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class Disasm extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Address addr = toAddr(args[0]);
        int count = Integer.parseInt(args[1]);
        disassemble(addr);
        Instruction insn = getInstructionAt(addr);
        for (int i = 0; i < count && insn != null; i++, insn = insn.getNext()) println(insn.getAddress() + "  " + insn);
    }
}
