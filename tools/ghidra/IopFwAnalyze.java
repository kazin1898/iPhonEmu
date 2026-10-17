// Analyze an EmbeddedIOP firmware image imported as a raw ARM binary at 0 (BinaryLoader, ARM:LE:32:v7).
// The image is an iBoot application: ARM-state vector table and exception handlers (words 8..15 hold the
// handler addresses), ARM startup at 0x40, and a Thumb main whose address (bit 0 set) is the header word
// at 0x30c. Disassembles each in its mode, then runs the analyzers so the Thumb call graph is followed.
// Headless:
//   analyzeHeadless PROJ n94 -process iopfw-10B329.bin -noanalysis -scriptPath DIR -postScript IopFwAnalyze.java
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.services.AnalysisPriority;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.mem.Memory;

public class IopFwAnalyze extends GhidraScript {
    private void entry(long a, boolean thumb, String name) throws Exception {
        Address addr = toAddr(a & ~1L);
        ArmDisassembleCommand cmd = new ArmDisassembleCommand(addr, null, thumb);
        cmd.applyTo(currentProgram, monitor);
        new CreateFunctionCmd(addr).applyTo(currentProgram, monitor);
        if (name != null) {
            createLabel(addr, name, true);
        }
        println("entry " + addr + (thumb ? " thumb " : " arm ") + (name == null ? "" : name));
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        String[] handlers = { "reset", "undef", "swi", "pabt", "dabt", "resv", "irq", "fiq" };
        entry(0, false, "vectors");
        for (int i = 0; i < 8; i++) {
            long h = mem.getInt(toAddr(0x20 + 4 * i)) & 0xffffffffL;
            entry(h, (h & 1) != 0, "exc_" + handlers[i]);
        }
        long main = mem.getInt(toAddr(0x30c)) & 0xffffffffL;
        entry(main, (main & 1) != 0, "main");
        long hook = mem.getInt(toAddr(0x334)) & 0xffffffffL;
        if (hook != 0xffffffffL && hook != 0) {
            entry(hook, (hook & 1) != 0, "early_hook");
        }
        analyzeAll(currentProgram);
        println("functions: " + currentProgram.getFunctionManager().getFunctionCount());
    }
}
