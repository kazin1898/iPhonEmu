// Decompile the functions containing the addresses given as script arguments. Every output line is
// prefixed "DC| " so a headless log can be filtered. Headless:
//   analyzeHeadless PROJ n94 -process kernelcache.mach -noanalysis -scriptPath DIR -postScript DecompileAt.java 0x...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.cmd.function.CreateFunctionCmd;

public class DecompileAt extends GhidraScript {
    private void out(String s) {
        for (String line : s.split("\n")) {
            println("DC| " + line);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            boolean create = arg.startsWith("+");
            Address a = toAddr(Long.parseLong(arg.replace("+", "").replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null && create) {
                CreateFunctionCmd cmd = new CreateFunctionCmd(a);
                cmd.applyTo(currentProgram, monitor);
                f = getFunctionContaining(a);
            }
            if (f == null) {
                out("==== " + arg + ": no function (prefix the address with + to create one there)");
                continue;
            }
            DecompileResults res = ifc.decompileFunction(f, 120, monitor);
            out("==== " + arg + " in " + f.getName() + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses() + " bytes)");
            if (res.decompileCompleted()) {
                out(res.getDecompiledFunction().getC());
            } else {
                out("decompile failed: " + res.getErrorMessage());
            }
        }
        ifc.dispose();
    }
}
