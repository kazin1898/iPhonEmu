// Disassemble prelinked kexts' __text sections as Thumb and run the (fast) analyzers, so their functions
// exist for DecompileAt. Reads ranges "0xVA 0xSIZE" per line from the file given as the script argument.
// The slow whole-program passes (Decompiler Parameter ID, Stack, Decompiler Switch Analysis) are turned
// off: on this kernelcache they ran for over an hour.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.lang.Register;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.math.BigInteger;
import java.util.List;

public class PrelinkAnalyze extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> lines = Files.readAllLines(Paths.get(getScriptArgs()[0]));
        Register tmode = currentProgram.getRegister("TMode");
        int n = 0;
        for (String line : lines) {
            String[] parts = line.trim().split("\\s+");
            if (parts.length < 2) continue;
            long va = Long.parseLong(parts[0].replace("0x", ""), 16);
            long size = Long.parseLong(parts[1].replace("0x", ""), 16);
            Address start = toAddr(va), end = toAddr(va + size - 1);
            try {
                currentProgram.getProgramContext().setValue(tmode, start, end, BigInteger.ONE);
            } catch (Exception e) {
                println("tmode " + parts[0] + ": " + e.getMessage());
            }
            AddressSet set = new AddressSet(start, end);
            DisassembleCommand dis = new DisassembleCommand(set, set, true);
            dis.applyTo(currentProgram, monitor);
            n++;
        }
        println("disassembled " + n + " kext text sections; running analyzers");
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "false");
        setAnalysisOption(currentProgram, "Stack", "false");
        setAnalysisOption(currentProgram, "Decompiler Switch Analysis", "false");
        analyzeAll(currentProgram);
        println("functions now: " + currentProgram.getFunctionManager().getFunctionCount());
    }
}
